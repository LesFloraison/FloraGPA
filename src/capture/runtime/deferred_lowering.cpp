#include "deferred_lowering.hpp"
#include <functional>
namespace flora::capture {
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool supported(CommandKind kind){switch(kind){
case CommandKind::draw:case CommandKind::draw_indexed:case CommandKind::draw_instanced:case CommandKind::draw_indexed_instanced:case CommandKind::dispatch:
case CommandKind::clear_rtv:case CommandKind::clear_dsv:case CommandKind::clear_uav_uint:case CommandKind::clear_uav_float:
case CommandKind::copy_resource:case CommandKind::copy_region:case CommandKind::update_subresource:return true;
default:return false;
}}
}
DeferredExpansion lower_deferred(const DeferredCommandList& list,const DeferredOrigin& origin,ObjectId context,size_t command_limit,size_t payload_limit){
    require(context && origin.list && origin.source_context==list.context && origin.generation==list.generation,"Deferred execution provenance mismatch");
    if(!list.gaps.empty())throw std::runtime_error("Deferred compilation gap: "+list.gaps.front());
    DeferredExpansion out;std::set<ObjectId> visiting;
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
        if(call.kind==CommandKind::binding){require(call.binding.has_value(),"Deferred binding arguments missing");continue;}
        require(supported(call.kind),"Deferred action lowering unsupported (Map rename/query/nested list or unknown action)");
        require(out.commands.size()<command_limit,"Deferred expansion command limit exceeded");
        require(call.state && call.completed,"Deferred action state/data incomplete");
        require(call.data.size()<=payload_limit-out.payload_bytes,"Deferred expansion payload byte limit exceeded");
        bindings(*call.state);retain(call.operand);retain(call.source);
        RecordedCommand c;c.kind=call.kind;c.api=call.api;c.words=call.words;c.floats=call.floats;c.operand=call.operand;c.source=call.source;c.update_box=call.update_box;c.copy_region=call.copy_region;c.data=call.data;c.result=call.result;c.completed=true;
        c.bindings=*call.state;c.bindings.context=context;c.deferred=origin;c.deferred->call_sequence=call.sequence;
        out.payload_bytes+=c.data.size();out.commands.push_back(std::move(c));
    }
    return out;
}
}
