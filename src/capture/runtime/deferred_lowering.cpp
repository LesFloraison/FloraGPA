#include "deferred_lowering.hpp"
#include "resource_layout.hpp"
#include <functional>
namespace flora::capture {
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>T descriptor(const FrozenObject& object){
    require(object.descriptor.size()==sizeof(T),"Deferred texture Map descriptor size mismatch");
    T value{};memcpy(&value,object.descriptor.data(),sizeof value);return value;
}
void texture_discard(const FrozenObject& object,const DeferredCall& call){
    require(call.words[1]==D3D11_MAP_WRITE_DISCARD && call.before_data.empty(),"Deferred texture Map requires WRITE_DISCARD");
    ResourceShape shape;
    auto common=[&](const auto& d){
        require(d.Usage==D3D11_USAGE_DYNAMIC && d.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE &&
                !(d.BindFlags&~D3D11_BIND_SHADER_RESOURCE) && !d.MiscFlags && d.MipLevels==1,
                "Deferred texture discard requires a single mip and ordinary dynamic storage");
        shape.width=d.Width;shape.format=d.Format;
    };
    auto two=[&](const auto& d){common(d);require(d.ArraySize==1 && d.SampleDesc.Count==1 && !d.SampleDesc.Quality,
        "Deferred texture discard array/MSAA profile unsupported");shape.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE2D;shape.height=d.Height;};
    auto three=[&](const auto& d){common(d);shape.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE3D;shape.height=d.Height;shape.depth=d.Depth;};
    if(object.kind=="texture1d"){const auto d=descriptor<D3D11_TEXTURE1D_DESC>(object);common(d);require(d.ArraySize==1,"Deferred texture discard array profile unsupported");shape.dimension=D3D11_RESOURCE_DIMENSION_TEXTURE1D;}
    else if(object.kind=="texture2d")two(descriptor<D3D11_TEXTURE2D_DESC>(object));
    else if(object.kind=="texture3d")three(descriptor<D3D11_TEXTURE3D_DESC>(object));
    else if(object.kind=="texture2d1"){const auto d=descriptor<D3D11_TEXTURE2D_DESC1>(object);require(d.TextureLayout==D3D11_TEXTURE_LAYOUT_UNDEFINED,"Deferred explicit texture layout unsupported");two(d);}
    else if(object.kind=="texture3d1"){const auto d=descriptor<D3D11_TEXTURE3D_DESC1>(object);require(d.TextureLayout==D3D11_TEXTURE_LAYOUT_UNDEFINED,"Deferred explicit texture layout unsupported");three(d);}
    else throw std::runtime_error("Deferred Map resource kind unsupported");
    require(!(shape.format>=DXGI_FORMAT_NV12 && shape.format<=DXGI_FORMAT_P016),"Deferred planar Map layout unsupported");
    const auto sub=resource_layout(shape,call.data.size()).at(0);
    require(sub.slice_bytes*sub.depth==call.data.size(),"Deferred texture Map payload length mismatch");
    const size_t row=call.words[3],slice=call.words[4];
    require(sub.rows==1 || row>=sub.row_bytes,"Deferred texture Map row pitch is too small");
    require(!row || sub.rows-1<=(SIZE_MAX-sub.row_bytes)/row,"Deferred texture Map row span overflow");
    const size_t span=(sub.rows-1)*row+sub.row_bytes;
    require(sub.depth==1 || slice>=span,"Deferred texture Map depth pitch is too small");
    require(span<=size_t(PTRDIFF_MAX) && (!slice || sub.depth-1<=(size_t(PTRDIFF_MAX)-span)/slice),"Deferred texture Map pointer span overflow");
}
bool supported(CommandKind kind){switch(kind){
case CommandKind::draw:case CommandKind::draw_indexed:case CommandKind::draw_instanced:case CommandKind::draw_indexed_instanced:case CommandKind::dispatch:
case CommandKind::clear_rtv:case CommandKind::clear_dsv:case CommandKind::clear_uav_uint:case CommandKind::clear_uav_float:
case CommandKind::copy_resource:case CommandKind::copy_region:case CommandKind::update_subresource:return true;
case CommandKind::map:case CommandKind::unmap:return true;
default:return false;
}}
DeferredExpansion expand(const DeferredCommandList& list,const DeferredOrigin& origin,ObjectId context,size_t command_limit,size_t payload_limit,
                         size_t depth,size_t& visits,std::set<const DeferredCommandList*>& active){
    require(depth<=DeferredHistory::nesting_limit,"Deferred merge nesting limit exceeded");
    require(active.insert(&list).second,"Deferred merge cycle");
    require(context && origin.list && origin.source_context==list.context && origin.generation==list.generation,"Deferred execution provenance mismatch");
    if(!list.gaps.empty())throw std::runtime_error("Deferred compilation gap: "+list.gaps.front());
    DeferredExpansion out;std::set<ObjectId> visiting;
    std::map<ObjectId,std::shared_ptr<const PipelineBindings>> mappings;
    // The vector is immutable list-owned storage, not native memory. GPU writes
    // invalidate this CPU baseline, so same-value writes cannot be lost in a diff.
    std::map<ObjectId,const std::vector<uint8_t>*> mapped_contents;
    std::function<void(ObjectId)> retain=[&](ObjectId id){
        if(!id || out.definitions.contains(id))return;
        require(visiting.insert(id).second,"Deferred definition cycle");
        const auto found=list.definitions.find(id);require(found!=list.definitions.end(),"Deferred action definition missing");const auto& object=found->second.object;
        require(object.id==id && (object.kind=="device"?id==list.device:object.device==list.device),"Deferred definition identity/device mismatch");
        require(object.kind!="context" && object.kind!="command_list" && !object.class_linkage,"Deferred definition kind/linkage unsupported");
        retain(object.device);retain(object.resource);
        if(object.kind=="buffer" || object.kind.starts_with("texture"))out.resources.insert(id);
        out.definitions.emplace(id,object);visiting.erase(id);
    };
    auto bindings=[&](const PipelineBindings& s){
        require(s.context==list.context && s.device==list.device,"Deferred action state context/device mismatch");
        require(!s.predicate && s.resource_min_lod.empty(),"Deferred execution-time predicate/MinLOD unsupported");
        retain(s.device);
        for(const auto& stage:s.shaders){require(stage.classes.empty(),"Deferred shader classes unsupported");retain(stage.shader);for(auto id:stage.buffers)retain(id);for(auto id:stage.resources)retain(id);for(auto id:stage.samplers)retain(id);}
        for(auto id:s.vertices)retain(id);for(auto id:s.render_targets)retain(id);
        for(auto id:s.stream_targets)require(!id,"Deferred SO append state unsupported");
        auto uav=[&](ObjectId id){if(!id)return;retain(id);const auto& d=out.definitions.at(id).descriptor;require(d.size()>=sizeof(D3D11_UNORDERED_ACCESS_VIEW_DESC),"Deferred UAV descriptor missing");D3D11_UNORDERED_ACCESS_VIEW_DESC value{};memcpy(&value,d.data(),sizeof value);require(value.ViewDimension!=D3D11_UAV_DIMENSION_BUFFER || !(value.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER)),"Deferred hidden UAV counter unsupported");};
        for(auto id:s.om_uavs)uav(id);for(auto id:s.cs_uavs)uav(id);
        for(auto id:{s.input_layout,s.index_buffer,s.rasterizer,s.blend,s.depth_stencil,s.depth_view})retain(id);
    };
    for(const auto& call:list.calls){
        require(++visits<=DeferredHistory::call_limit,"Deferred merge traversal limit exceeded");
        if(call.merged_list){
            require(call.kind==CommandKind::unsupported && call.api.ends_with("::ExecuteCommandList") && call.operand && !call.source &&
                    call.completed && call.result==S_OK && call.data.empty() && call.before_data.empty() &&
                    !call.words[1] && !call.words[2] && !call.words[3] && !call.words[4],"Deferred merge arguments invalid");
            require(mappings.empty(),"Deferred merge with open mappings is unverified");
            const auto& child=*call.merged_list;
            require(child.device==list.device,"Deferred merge device mismatch");
            DeferredOrigin nested{call.operand,child.context,child.generation,call.sequence+1,0,call.words[0]!=0};
            auto added=expand(child,nested,context,command_limit-out.commands.size(),payload_limit-out.payload_bytes,depth+1,visits,active);
            for(auto& command:added.commands){
                if(command.kind==CommandKind::map || command.kind==CommandKind::copy_resource ||
                   command.kind==CommandKind::copy_region || command.kind==CommandKind::update_subresource)
                    mapped_contents.erase(command.operand);
                command.merged_origins.insert(command.merged_origins.begin(),*command.deferred);
                command.deferred=origin;command.deferred->call_sequence=call.sequence;
                out.commands.push_back(std::move(command));
            }
            out.resources.insert(added.resources.begin(),added.resources.end());
            out.definitions.merge(added.definitions);out.payload_bytes+=added.payload_bytes;
            continue;
        }
        if(call.kind==CommandKind::binding){require(call.binding.has_value(),"Deferred binding arguments missing");continue;}
        require(supported(call.kind),"Deferred action lowering unsupported (Map rename/query/nested list or unknown action)");
        require(out.commands.size()<command_limit,"Deferred expansion command limit exceeded");
        auto state=call.state;
        if(call.kind==CommandKind::unmap){
            const auto found=mappings.find(call.operand);
            require(found!=mappings.end() && call.words==std::array<uint32_t,5>{} && call.data.empty() && call.before_data.empty() && call.result==S_OK,
                    "Deferred Unmap has no matching complete Map");
            state=found->second;mappings.erase(found);
        }
        require(state && call.completed,"Deferred action state/data incomplete");
        require(call.kind==CommandKind::map || call.before_data.empty(),"Deferred non-Map action has a CPU baseline");
        require(call.data.size()<=payload_limit-out.payload_bytes,"Deferred expansion payload byte limit exceeded");
        require(call.before_data.size()<=payload_limit-out.payload_bytes-call.data.size(),"Deferred expansion baseline byte limit exceeded");
        bindings(*state);retain(call.operand);retain(call.source);
        if(call.kind==CommandKind::map){
            require(call.operand && !call.source && call.result==S_OK && call.words[0]==0 &&
                    (call.words[1]==D3D11_MAP_WRITE_DISCARD || call.words[1]==D3D11_MAP_WRITE_NO_OVERWRITE) && !call.words[2],
                    "Deferred Map rename requires a complete zero-flag write");
            const auto& object=out.definitions.at(call.operand);
            if(object.kind=="buffer"){
            require(object.descriptor.size()==sizeof(D3D11_BUFFER_DESC),
                    "Deferred Map rename resource is not a verified buffer");
            D3D11_BUFFER_DESC desc{};memcpy(&desc,object.descriptor.data(),sizeof desc);
            require(desc.ByteWidth && desc.Usage==D3D11_USAGE_DYNAMIC && desc.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE &&
                    !desc.MiscFlags && !desc.StructureByteStride &&
                    !(desc.BindFlags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE)) &&
                    call.data.size()==desc.ByteWidth,"Deferred Map rename buffer descriptor/payload unsupported");
            }else texture_discard(object,call);
            require(mappings.emplace(call.operand,state).second,"Deferred Map rename overlaps an open mapping");
            if(call.words[1]==D3D11_MAP_WRITE_NO_OVERWRITE){
                const auto previous=mapped_contents.find(call.operand);
                require(previous!=mapped_contents.end() && call.before_data==*previous->second,
                        "Deferred NO_OVERWRITE baseline is not the preceding list-owned write");
            }else require(call.before_data.empty(),"Deferred discard has an unexpected baseline");
            mapped_contents[call.operand]=&call.data;
        }else if(call.kind==CommandKind::copy_resource || call.kind==CommandKind::copy_region || call.kind==CommandKind::update_subresource){
            mapped_contents.erase(call.operand);
        }
        RecordedCommand c;c.kind=call.kind;c.api=call.api;c.words=call.words;c.floats=call.floats;c.operand=call.operand;c.source=call.source;c.update_box=call.update_box;c.copy_region=call.copy_region;c.data=call.data;c.result=call.result;c.completed=true;
        c.before_data=call.before_data;c.bindings=*state;c.bindings.context=context;c.deferred=origin;c.deferred->call_sequence=call.sequence;
        out.payload_bytes+=c.data.size()+c.before_data.size();out.commands.push_back(std::move(c));
    }
    require(mappings.empty(),"Deferred Map rename has no matching Unmap");
    active.erase(&list);
    return out;
}
}
DeferredExpansion lower_deferred(const DeferredCommandList& list,const DeferredOrigin& origin,ObjectId context,size_t command_limit,size_t payload_limit){
    size_t visits=0;std::set<const DeferredCommandList*> active;
    return expand(list,origin,context,command_limit,payload_limit,0,visits,active);
}
}
