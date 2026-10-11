#include "encoder.hpp"
#include "resource_layout.hpp"
#include "query_dependencies.hpp"
#include "format/Writer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <objbase.h>
#include <span>
#include <stdexcept>

namespace flora::capture {
namespace {
using namespace flora::format;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
uint32_t narrow(size_t value){require(value<=UINT32_MAX,"Encoder wire size overflow");return uint32_t(value);}
struct Payload:ByteBuffer {
    template<class T>Payload& value(T v){const auto at=size();resize(at+sizeof(T));wire_write(data()+at,v);return *this;}
    Payload& q(std::initializer_list<uint64_t> values){for(auto v:values)value(v);return *this;}
    Payload& u(std::initializer_list<uint32_t> values){for(auto v:values)value(v);return *this;}
    Payload& bytes(std::span<const uint8_t> bytes){insert(end(),bytes.begin(),bytes.end());return *this;}
    template<class T>Payload& raw(const T& v){return bytes({reinterpret_cast<const uint8_t*>(&v),sizeof(v)});}
};
template<class T>T descriptor(const FrozenObject& object){
    require(object.descriptor.size()>=sizeof(T),"Encoder descriptor is truncated");T d;memcpy(&d,object.descriptor.data(),sizeof(d));return d;
}
struct Record{uint8_t category;uint16_t type;ByteBuffer bytes;};
class Encoder {
    const FrozenFrame& frame_;
    const EncodeOptions& options_;
    std::map<ObjectId,uint64_t> ids_;
    std::map<uint64_t,Record> records_;
    uint64_t next_=4;
    ObjectId device_=0,context_=0;
    void checkpoint(){if(options_.cancelled && options_.cancelled())throw std::runtime_error("Frame encoding cancelled");}
    uint64_t allocate(){require(next_<MAX_ENTRIES,"Encoder record count limit exceeded");return next_++;}
    void add(uint64_t id,uint8_t category,uint16_t type,ByteBuffer bytes){
        checkpoint();narrow(bytes.size());require(records_.emplace(id,Record{category,type,std::move(bytes)}).second,"Encoder duplicate record ID");
    }
    uint64_t add(uint8_t category,uint16_t type,ByteBuffer bytes){const auto id=allocate();add(id,category,type,std::move(bytes));return id;}
    uint64_t ref(ObjectId id,const char* kind=nullptr){
        if(!id)return 0;
        const auto found=frame_.objects.find(id);require(found!=frame_.objects.end() && ids_.contains(id),"Encoder object reference is missing");
        if(kind)require(found->second.kind==kind,"Encoder object reference has wrong kind");return ids_.at(id);
    }
    uint64_t typed(ObjectId id,std::initializer_list<const char*> kinds){
        const auto result=ref(id);if(!id)return result;
        const auto& kind=frame_.objects.at(id).kind;
        require(std::any_of(kinds.begin(),kinds.end(),[&](auto candidate){return kind==candidate;}),"Encoder object reference has wrong kind");return result;
    }
    uint64_t resource_ref(ObjectId id){return typed(id,{"buffer","texture1d","texture2d","texture2d1","texture3d","texture3d1"});}
    uint64_t blob(std::span<const uint8_t> bytes){return add(9,1,Payload{}.value(narrow(bytes.size())).bytes(bytes));}
    uint64_t mapped_diff(std::span<const uint8_t> before,std::span<const uint8_t> after){
        require(before.size()==after.size(),"Encoder NO_OVERWRITE baseline length mismatch");Payload ranges,bytes;
        for(size_t at=0;at<after.size();){
            checkpoint();if(before[at]==after[at]){++at;continue;}
            const auto begin=at;while(at<after.size() && before[at]!=after[at])++at;
            ranges.u({narrow(begin),narrow(at-begin)});bytes.bytes(after.subspan(begin,at-begin));
            require(bytes.size()<=UINT32_MAX-8 && ranges.size()<=UINT32_MAX-8-bytes.size(),"Encoder mapped diff size overflow");
        }
        return add(9,0x100,Payload{}.u({narrow(ranges.size()),narrow(bytes.size())}).bytes(ranges).bytes(bytes));
    }
    ByteBuffer contents(const ResourceContents& data){
        const auto layout=resource_layout(data.shape);require(layout.size()==data.subresources.size(),"Encoder subresource count mismatch");
        ByteBuffer bytes;
        for(size_t i=0;i<layout.size();++i){const auto& a=layout[i];const auto& b=data.subresources[i];
            require(a.index==b.index && a.mip==b.mip && a.array_slice==b.array_slice && a.width==b.width && a.height==b.height && a.depth==b.depth && a.row_bytes==b.row_bytes && a.rows==b.rows && a.slice_bytes==b.slice_bytes && b.bytes.size()==a.slice_bytes*a.depth,"Encoder subresource layout/length mismatch");
            require(bytes.size()<=UINT32_MAX-b.bytes.size(),"Encoder resource blob overflow");bytes.insert(bytes.end(),b.bytes.begin(),b.bytes.end());
        }return bytes;
    }
    void resource(ObjectId id,const FrozenObject& object){
        const auto data=frame_.initial_resources.find(id);require(data!=frame_.initial_resources.end(),"Encoder resource has no initial GPU contents");
        const auto& s=data->second.shape;Payload payload;payload.q({ref(id),id==frame_.output?3ull:2ull});uint16_t type=0;
        if(object.kind=="buffer"){
            const auto d=descriptor<D3D11_BUFFER_DESC>(object);require(s.dimension==D3D11_RESOURCE_DIMENSION_BUFFER && s.width==d.ByteWidth && s.misc_flags==d.MiscFlags,"Encoder buffer descriptor/shape mismatch");payload.raw(d);type=0x83;
        }else if(object.kind=="texture1d"){
            const auto d=descriptor<D3D11_TEXTURE1D_DESC>(object);require(s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D && s.width==d.Width && s.mip_levels==d.MipLevels && s.array_size==d.ArraySize && s.format==d.Format && s.misc_flags==d.MiscFlags,"Encoder texture1D descriptor/shape mismatch");payload.raw(d);type=0x84;
        }else if(object.kind=="texture2d" || object.kind=="texture2d1"){
            if(object.kind=="texture2d1")require(descriptor<D3D11_TEXTURE2D_DESC1>(object).TextureLayout==D3D11_TEXTURE_LAYOUT_UNDEFINED,"Explicit texture layout is not encoded");
            const auto d=descriptor<D3D11_TEXTURE2D_DESC>(object);require(s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D && s.width==d.Width && s.height==d.Height && s.mip_levels==d.MipLevels && s.array_size==d.ArraySize && s.samples==d.SampleDesc.Count && s.format==d.Format && s.misc_flags==d.MiscFlags,"Encoder texture2D descriptor/shape mismatch");payload.raw(d);type=0x85;
        }else{
            if(object.kind=="texture3d1")require(descriptor<D3D11_TEXTURE3D_DESC1>(object).TextureLayout==D3D11_TEXTURE_LAYOUT_UNDEFINED,"Explicit texture layout is not encoded");
            const auto d=descriptor<D3D11_TEXTURE3D_DESC>(object);require(s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE3D && s.width==d.Width && s.height==d.Height && s.depth==d.Depth && s.mip_levels==d.MipLevels && s.format==d.Format && s.misc_flags==d.MiscFlags,"Encoder texture3D descriptor/shape mismatch");payload.raw(d);type=0x86;
        }
        constexpr UINT allowed=D3D11_RESOURCE_MISC_GENERATE_MIPS|D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS|D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS|D3D11_RESOURCE_MISC_BUFFER_STRUCTURED|D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
        require(!(s.misc_flags&~allowed),"Encoder special/shared resource flags unsupported");
        payload.value(blob(contents(data->second)));add(ref(id),5,type,std::move(payload));
    }
    void object(ObjectId id,const FrozenObject& o){
        if(id==device_ || id==context_)return;
        require(o.device==device_,"Encoder object belongs to another device");
        if(o.kind=="buffer" || o.kind=="texture1d" || o.kind=="texture2d" || o.kind=="texture2d1" || o.kind=="texture3d" || o.kind=="texture3d1"){resource(id,o);return;}
        Payload p;p.q({ref(id),2});uint16_t type=0;
        if(o.resource){
            require(frame_.initial_resources.contains(o.resource),"Encoder view source has no resource data");p.value(resource_ref(o.resource));
            if(o.kind=="srv" || o.kind=="srv1"){
                if(o.kind=="srv1"){auto d=descriptor<D3D11_SHADER_RESOURCE_VIEW_DESC1>(o);if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)require(!d.Texture2D.PlaneSlice,"SRV plane selection is not encoded");if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY)require(!d.Texture2DArray.PlaneSlice,"SRV plane selection is not encoded");}
                p.raw(descriptor<D3D11_SHADER_RESOURCE_VIEW_DESC>(o));type=0x8c;
            }else if(o.kind=="rtv" || o.kind=="rtv1"){
                if(o.kind=="rtv1"){auto d=descriptor<D3D11_RENDER_TARGET_VIEW_DESC1>(o);if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D)require(!d.Texture2D.PlaneSlice,"RTV plane selection is not encoded");if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY)require(!d.Texture2DArray.PlaneSlice,"RTV plane selection is not encoded");}
                p.raw(descriptor<D3D11_RENDER_TARGET_VIEW_DESC>(o));type=0x8d;
            }else if(o.kind=="dsv"){p.raw(descriptor<D3D11_DEPTH_STENCIL_VIEW_DESC>(o));type=0x8e;}
            else if(o.kind=="uav" || o.kind=="uav1"){
                if(o.kind=="uav1"){auto d=descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC1>(o);if(d.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2D)require(!d.Texture2D.PlaneSlice,"UAV plane selection is not encoded");if(d.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2DARRAY)require(!d.Texture2DArray.PlaneSlice,"UAV plane selection is not encoded");}
                p.raw(descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC>(o));type=0x8f;
            }else throw std::runtime_error("Unsupported view wire kind: "+o.kind);
        }else if(o.kind.ends_with("_shader")){
            const std::map<std::string,UINT> types{{"vertex_shader",0x90},{"geometry_shader",0x91},{"pixel_shader",0x92},{"compute_shader",0x93},{"domain_shader",0x94},{"hull_shader",0x95}};
            require(types.contains(o.kind) && o.creation_observed && o.creation_bytes.size()>=4 && !memcmp(o.creation_bytes.data(),"DXBC",4),"Encoder shader DXBC unavailable");
            require(!o.class_linkage && o.stream_elements.empty(),"Shader linkage/stream-output encoding is pending");type=uint16_t(types.at(o.kind));
            // Auxiliary words remain opaque. Zero is verified only for the tested shader profile.
            const auto data=add(9,0x81,Payload{}.value<uint64_t>(o.creation_bytes.size()).bytes(o.creation_bytes).q({0}));p.q({0,0,0,0,data});
        }else if(o.kind=="input_layout"){
            require(o.creation_observed && o.input_elements.size()<=32 && !o.creation_bytes.empty(),"Input layout creation data unavailable");Payload layout;layout.value(narrow(o.input_elements.size()));
            for(const auto& e:o.input_elements){require(e.semantic.find('\0')==std::string::npos,"Input semantic contains NUL");ByteBuffer name(e.semantic.begin(),e.semantic.end());name.push_back(0);layout.value(blob(name)).u({e.semantic_index,e.format,e.slot,e.offset,e.slot_class,e.step_rate});}
            layout.value(narrow(o.creation_bytes.size())).bytes(o.creation_bytes);p.value(add(9,0x84,std::move(layout)));type=0x82;
        }else if(o.kind=="sampler"){p.raw(descriptor<D3D11_SAMPLER_DESC>(o));type=0x88;}
        else if(o.kind=="depth_stencil"){p.raw(descriptor<D3D11_DEPTH_STENCIL_DESC>(o));type=0x8b;}
        else if(o.kind=="rasterizer"){p.raw(descriptor<D3D11_RASTERIZER_DESC>(o));type=0x89;}
        else if(o.kind=="rasterizer1"){p.raw(descriptor<D3D11_RASTERIZER_DESC1>(o));type=0x10e;}
        else if(o.kind=="rasterizer2"){p.raw(descriptor<D3D11_RASTERIZER_DESC2>(o));type=0x10f;}
        else if(o.kind=="blend"){p.raw(descriptor<D3D11_BLEND_DESC>(o));type=0x8a;}
        else if(o.kind=="blend1"){p.raw(descriptor<D3D11_BLEND_DESC1>(o));type=0x10d;}
        else throw std::runtime_error("Unsupported object wire kind: "+o.kind);
        add(ref(id),5,type,std::move(p));
    }
    template<size_t N>void refs(const std::array<ObjectId,N>& from,std::array<uint64_t,N>& to,std::initializer_list<const char*> kinds){for(size_t i=0;i<N;++i)to[i]=typed(from[i],kinds);}
    uint64_t state(const RecordedCommand& command){
        const auto& s=command.bindings;require(s.context==context_ && s.device==device_,"Encoder mixed context/device state");
        require(!s.predicate && std::all_of(s.stream_targets.begin(),s.stream_targets.end(),[](auto id){return id==0;}),"Predicate/SO state encoding pending");
        for(const auto& [id,lod]:s.resource_min_lod){resource_ref(id);require(lod==0,"Nonzero resource minimum LOD encoding pending");}
        state_block out{};out.mask.fill(UINT32_MAX);out.mask[672/32]&=~(1u<<(672%32));
        out.ib=ref(s.index_buffer,"buffer");out.ib_format=s.index_format;out.ib_offset=s.index_offset;out.input_layout=ref(s.input_layout,"input_layout");out.topology=s.topology;
        refs(s.vertices,out.vb,{"buffer"});out.strides=s.strides;out.offsets=s.offsets;
#define STAGE(NAME,INDEX) {const auto& stage=s.shaders[size_t(ShaderStage::INDEX)];require(stage.classes.empty(),"Dynamic class state encoding pending"); \
    for(size_t i=0;i<stage.buffers.size();++i)if(stage.buffers[i])require(!stage.first_constants[i] && (!s.constant_ranges || stage.constant_counts[i]==4096),"Constant-buffer range encoding pending"); \
    refs(stage.buffers,out.NAME##_cb,{"buffer"});refs(stage.samplers,out.NAME##_samplers,{"sampler"});refs(stage.resources,out.NAME##_srv,{"srv","srv1"});out.NAME##_shader=ref(stage.shader,#INDEX "_shader");}
        STAGE(vs,vertex) STAGE(hs,hull) STAGE(ds,domain) STAGE(gs,geometry) STAGE(ps,pixel) STAGE(cs,compute)
#undef STAGE
        require(s.viewports.size()<=16 && s.scissors.size()<=16,"Encoder viewport/scissor limit exceeded");
        Payload viewports;viewports.value(narrow(s.viewports.size()));for(const auto& v:s.viewports)viewports.raw(v);out.viewports=add(9,0x87,std::move(viewports));
        Payload scissors;scissors.value(narrow(s.scissors.size()));for(const auto& v:s.scissors)scissors.raw(v);out.scissors=add(9,0x86,std::move(scissors));
        out.rasterizer=typed(s.rasterizer,{"rasterizer","rasterizer1","rasterizer2"});out.blend=typed(s.blend,{"blend","blend1"});out.blend_factor=s.blend_factor;out.sample_mask=s.sample_mask;out.depth_state=ref(s.depth_stencil,"depth_stencil");out.stencil_ref=s.stencil_ref;out.dsv=ref(s.depth_view,"dsv");
        for(const auto& [id,count]:command.uav_counters){(void)count;require(id!=0,"Encoder counter identity is null");typed(id,{"uav","uav1"});}
        auto require_counter=[&](ObjectId id){if(!id)return;typed(id,{"uav","uav1"});const auto d=descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC>(frame_.objects.at(id));
            if(d.ViewDimension==D3D11_UAV_DIMENSION_BUFFER && (d.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER)))
                require(command.uav_counters.contains(id),"Encoder hidden UAV counter is missing");};
        out.om_uav_start=64;out.om_uav_counts.fill(UINT32_MAX);out.cs_uav_counts.fill(UINT32_MAX);
        for(size_t i=0;i<64;++i){require(i<8 || (!s.om_uavs[i] && !s.cs_uavs[i]),"Extended UAV wire validation pending");if(i>=8)continue;
            require_counter(s.om_uavs[i]);require_counter(s.cs_uavs[i]);
            out.rtv[i]=typed(s.render_targets[i],{"rtv","rtv1"});if(out.rtv[i])out.rt_count=UINT(i+1);
            if(s.om_uavs[i]){require(!s.render_targets[i],"Overlapping RTV/UAV state");out.rtv[i]=typed(s.om_uavs[i],{"uav","uav1"});out.om_uav_start=(std::min)(out.om_uav_start,UINT(i));out.rt_count=UINT(i+1);}
            out.cs_uav[i]=typed(s.cs_uavs[i],{"uav","uav1"});if(out.cs_uav[i])out.cs_uav_count=UINT(i+1);
            if(auto c=command.uav_counters.find(s.om_uavs[i]);c!=command.uav_counters.end())out.om_uav_counts[i]=c->second;
            if(auto c=command.uav_counters.find(s.cs_uavs[i]);c!=command.uav_counters.end())out.cs_uav_counts[i]=c->second;
        }
        return add(3,3,Payload{}.bytes(encode_state_block(out)));
    }
public:
    Encoder(const FrozenFrame& frame,const EncodeOptions& options):frame_(frame),options_(options){}
    void write(const std::filesystem::path& path){
        checkpoint();
        for(const auto& command:frame_.commands)
            if(command.kind==CommandKind::query_end || command.kind==CommandKind::query_get_data){
                EventDependencyOptions analysis_options;analysis_options.cancelled=options_.cancelled;
                const auto dependencies=analyze_event_dependencies(frame_,analysis_options);
                std::string detail="; End observations="+std::to_string(dependencies.ends)+", GetData observations="+std::to_string(dependencies.observations.size());
                std::array<size_t,7> scopes{};
                const EventDependencyObservation* latest=nullptr;
                for(const auto& observed:dependencies.observations){
                    ++scopes.at(size_t(observed.scope));
                    if(observed.prefix && (!latest || observed.end_sequence>latest->end_sequence))latest=&observed;
                }
                constexpr const char* scope_names[]{"pending","failed","unissued","pre-frame","deferred","foreign-context","recorded-prefix"};
                for(size_t i=0;i<scopes.size();++i)if(scopes[i])detail+=", "+std::string(scope_names[i])+"="+std::to_string(scopes[i]);
                if(latest){
                    const auto& prefix=*latest->prefix;
                    detail+="; latest recorded End="+std::to_string(*latest->end_sequence)+", candidate copy waits="+std::to_string(prefix.copies.size())+", uncovered commands="+std::to_string(prefix.uncovered_commands);
                    if(prefix.first_uncovered_sequence)detail+="; first uncovered sequence="+std::to_string(*prefix.first_uncovered_sequence)+": "+prefix.first_uncovered_reason;
                }
                detail+="; whole EVENT completion remains unverified";
                throw std::runtime_error("EVENT query serialization is not verified; frame not published ("+
                    command.api+", object="+std::to_string(command.operand)+", sequence="+std::to_string(command.sequence)+")"+detail);
            }
        require(!frame_.commands.empty(),"Encoder requires recorded commands");context_=frame_.commands.front().bindings.context;device_=frame_.commands.front().bindings.device;
        require(context_ && device_ && context_!=device_,"Encoder invalid context/device identities");
        ids_.emplace(context_,1);ids_.emplace(device_,2);
        for(const auto& [id,o]:frame_.objects){require(id && id==o.id,"Encoder object key/identity mismatch");if(!ids_.contains(id))ids_.emplace(id,allocate());}
        ref(context_,"context");ref(device_,"device");const auto device=descriptor<DeviceRecordDesc>(frame_.objects.at(device_));
        require(device.feature_level>=D3D_FEATURE_LEVEL_11_0 && device.feature_level<=D3D_FEATURE_LEVEL_11_1,"Encoder feature level unsupported");
        const auto context=descriptor<ContextRecordDesc>(frame_.objects.at(context_));require(!context.type,"Encoder deferred context unsupported");
        require(frame_.objects.at(context_).device==device_,"Encoder context device mismatch");
        typed(frame_.output,{"texture2d","texture2d1"});const auto output=frame_.objects.find(frame_.output);require(output!=frame_.objects.end() && frame_.initial_resources.contains(frame_.output),"Encoder output missing");
        const auto& shape=frame_.final_output.shape;
        require(shape.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D && shape.mip_levels==1 && shape.array_size==1 && shape.samples==1 && (shape.format==DXGI_FORMAT_R8G8B8A8_UNORM || shape.format==DXGI_FORMAT_B8G8R8A8_UNORM),"Encoder output profile unsupported");
        const auto final=contents(frame_.final_output);const auto output_desc=descriptor<D3D11_TEXTURE2D_DESC>(output->second);
        require(output_desc.MipLevels==1 && output_desc.ArraySize==1 && output_desc.SampleDesc.Count==1 && output_desc.SampleDesc.Quality==0,"Encoder output descriptor profile unsupported");
        require(shape.width==output_desc.Width && shape.height==output_desc.Height && shape.format==output_desc.Format,"Encoder final output descriptor mismatch");
        add(1,5,0x127,Payload{}.q({1,2,0}));add(2,5,0x81,Payload{}.q({2,0}).u({1,0,device.feature_level}));
        Payload swap;swap.resize(88);wire_write<uint64_t>(swap.data(),3);wire_write<uint64_t>(swap.data()+8,2);
        const UINT usage=DXGI_USAGE_RENDER_TARGET_OUTPUT|((output_desc.BindFlags&D3D11_BIND_UNORDERED_ACCESS)?DXGI_USAGE_UNORDERED_ACCESS:0);
        for(auto [at,value]:{std::pair{16u,shape.width},{20u,shape.height},{32u,UINT(shape.format)},{44u,1u},{52u,usage},{56u,1u},{72u,1u}})wire_write(swap.data()+at,value);
        if(frame_.presentation){
            const auto& d=frame_.presentation->descriptor;
            require(d.BufferDesc.Width==shape.width && d.BufferDesc.Height==shape.height && d.BufferDesc.Format==shape.format && d.SampleDesc.Count==1 && d.SampleDesc.Quality==0,"Encoder presentation/output mismatch");
            require(supported_swap_buffers(d.SwapEffect,d.BufferCount),"Unsupported swap effect/buffer count");
            require(!(frame_.presentation->flags&~DXGI_PRESENT_ALLOW_TEARING),"Unsupported encoded Present flags");
            if(frame_.presentation->flags&DXGI_PRESENT_ALLOW_TEARING)require((d.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) && d.Windowed && frame_.presentation->sync_interval==0 && (d.SwapEffect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || d.SwapEffect==DXGI_SWAP_EFFECT_FLIP_DISCARD),"Invalid tearing presentation configuration");
            static_assert(sizeof(DXGI_SWAP_CHAIN_DESC)==72);
            memcpy(swap.data()+16,&d,sizeof(d));
        }
        add(3,5,0x38,std::move(swap));
        for(const auto& [id,o]:frame_.objects)object(id,o);
        for(const auto& [id,data]:frame_.initial_resources){(void)data;require(frame_.objects.contains(id),"Encoder orphan initial resource");resource_ref(id);}
        std::map<std::pair<ObjectId,UINT>,bool> mappings;
        uint64_t sequence=0;
        for(const auto& command:frame_.commands){
            require(command.sequence==sequence++,"Encoder command sequence mismatch");const auto& w=command.words;Payload p;uint16_t type=0;
            if(command.kind==CommandKind::map || command.kind==CommandKind::unmap){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder Map context mismatch");
                require(command.completed && command.operand,"Encoder incomplete Map/Unmap");const auto resource=resource_ref(command.operand);
                require(frame_.initial_resources.contains(command.operand),"Encoder Map resource data missing");
                const auto& mapped_shape=frame_.initial_resources.at(command.operand).shape;const auto layout=resource_layout(mapped_shape);
                require(w[0]<layout.size(),"Encoder Map subresource out of range");const auto key=std::pair{command.operand,w[0]};
                if(command.kind==CommandKind::unmap){
                    require(mappings.erase(key)==1 && command.data.empty(),"Encoder Unmap has no successful Map");
                    add(7,0x34ed,Payload{}.q({0,1,resource}).value(w[0]));continue;
                }
                require(w[1]>=1 && w[1]<=5 && !(w[2]&~D3D11_MAP_FLAG_DO_NOT_WAIT),"Encoder invalid Map type/flags");
                uint64_t data=0;
                if(command.result<0)require(command.data.empty() && command.before_data.empty(),"Encoder failed Map has data");
                else{
                    require(command.result==S_OK && (!w[2] || w[1]<D3D11_MAP_WRITE_DISCARD),"Encoder invalid successful Map");
                    require(!(mapped_shape.format>=DXGI_FORMAT_NV12 && mapped_shape.format<=DXGI_FORMAT_P016),"Encoder planar Map layout unverified");
                    require(mapped_shape.dimension==D3D11_RESOURCE_DIMENSION_BUFFER || w[1]!=D3D11_MAP_WRITE_NO_OVERWRITE,"Encoder texture NO_OVERWRITE unsupported");
                    const auto& sub=layout.at(w[0]);require(command.data.size()==sub.slice_bytes*sub.depth,"Encoder Map data length mismatch");
                    require(mappings.emplace(key,true).second,"Encoder overlapping Map calls");
                    if(w[1]==D3D11_MAP_WRITE_NO_OVERWRITE)data=mapped_diff(command.before_data,command.data);
                    else{require(command.before_data.empty(),"Encoder unexpected Map baseline");data=blob(command.data);}
                }
                add(7,w[1]==D3D11_MAP_READ?0x34ec:0x246,Payload{}.q({0,1}).value(command.result).value(resource).u({w[0],w[1],w[2]}).value(data));continue;
            }
            require(mappings.empty(),"Encoder GPU execution with open Map unsupported");
            if(command.kind==CommandKind::copy_resource){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder copy context mismatch");
                require(!command.bindings.predicate,"Encoder predicated copy is unsupported");
                require(command.operand && command.source && command.operand!=command.source,"Encoder copy operands are missing or alias");
                const auto destination=resource_ref(command.operand),source=resource_ref(command.source);
                const auto& d=frame_.initial_resources.at(command.operand);
                validate_copy_resource(d.shape,frame_.objects.at(command.operand).descriptor,frame_.initial_resources.at(command.source).shape);
                add(7,0x3e,Payload{}.q({0,1,destination,source}));continue;
            }
            switch(command.kind){
            case CommandKind::draw:type=0x37;break;case CommandKind::draw_indexed:type=0x39;break;
            case CommandKind::draw_instanced:type=0x3c;break;case CommandKind::draw_indexed_instanced:type=0x3a;break;case CommandKind::dispatch:type=0x35;break;
            case CommandKind::clear_rtv:type=0x32;break;case CommandKind::clear_dsv:type=0x31;break;case CommandKind::clear_uav_uint:type=0x33;break;case CommandKind::clear_uav_float:type=0x34;break;
            default:throw std::runtime_error("Unsupported command wire kind");
            }
            if(type>=0x35){p.q({state(command),0,1});const UINT count=type==0x37?2:type==0x3c?4:type==0x3a?5:3;for(UINT i=0;i<count;++i)p.value(w[i]);}
            else{require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder clear context mismatch");require(command.operand!=0,"Encoder clear operand missing");const auto view=type==0x31?typed(command.operand,{"dsv"}):type==0x32?typed(command.operand,{"rtv","rtv1"}):typed(command.operand,{"uav","uav1"});p.q({0,1,view});
                if(type==0x31){require((w[0]&~3u)==0 && w[1]<=255,"Encoder invalid depth/stencil clear arguments");p.value(w[0]).value(command.floats[0]).value(uint8_t(w[1]));}
                else{p.value<uint8_t>(1);if(type==0x33)for(size_t i=0;i<4;++i)p.value(w[i]);else for(auto v:command.floats)p.value(v);}
            }add(7,type,std::move(p));
        }
        require(mappings.empty(),"Encoder frame has unpaired Map");
        const auto reference_data=blob(final);add(5,0x87,Payload{}.q({UINT64_MAX-1,3}).raw(output_desc).value(reference_data));
        add(7,0x3257,Payload{}.q({0,3}).u({0,frame_.presentation?frame_.presentation->sync_interval:1,frame_.presentation?frame_.presentation->flags:0}));
        Header header{};memcpy(header.data(),"IGPA",4);wire_write<uint32_t>(header.data()+4,HEADER_SIZE);wire_write<uint32_t>(header.data()+8,VERSION);
        GUID guid{};require(SUCCEEDED(CoCreateGuid(&guid)),"Frame GUID generation failed");memcpy(header.data()+0x20,&guid,16);memcpy(header.data()+API_OFFSET,"DX11",4);memcpy(header.data()+0x4c,"DX11_0",6);
        require(frame_.producer.size()<32 && frame_.producer.find('\0')==std::string::npos,"Encoder producer name is invalid");memcpy(header.data()+0xd4,frame_.producer.data(),frame_.producer.size());wire_write<uint32_t>(header.data()+0xfc,6);
        wire_write(header.data()+DIMENSIONS_OFFSET,shape.width);wire_write(header.data()+DIMENSIONS_OFFSET+4,shape.height);
        ByteBuffer bmp(54+final.size());bmp[0]='B';bmp[1]='M';wire_write(bmp.data()+2,narrow(bmp.size()));wire_write<uint32_t>(bmp.data()+10,54);wire_write<uint32_t>(bmp.data()+14,40);wire_write(bmp.data()+18,shape.width);wire_write<int32_t>(bmp.data()+22,-int32_t(shape.height));wire_write<uint16_t>(bmp.data()+26,1);wire_write<uint16_t>(bmp.data()+28,32);wire_write(bmp.data()+34,narrow(final.size()));
        for(size_t i=0;i<final.size();i+=4){bmp[54+i]=final[i+(shape.format==DXGI_FORMAT_R8G8B8A8_UNORM?2:0)];bmp[55+i]=final[i+1];bmp[56+i]=final[i+(shape.format==DXGI_FORMAT_R8G8B8A8_UNORM?0:2)];bmp[57+i]=final[i+3];}
        checkpoint();ContainerWriter writer(path,header);for(const auto& [id,r]:records_){checkpoint();writer.append({id,r.category,r.type,r.bytes.data(),r.bytes.size()});}
        std::unique_lock<std::mutex> publication;
        if(options_.publication_mutex)publication=std::unique_lock<std::mutex>(*options_.publication_mutex);
        checkpoint();writer.finish(bmp);
        if(options_.published)options_.published->store(true);
    }
};
}
void encode_frame(const FrozenFrame& frame,const std::filesystem::path& path,const EncodeOptions& options){Encoder(frame,options).write(path);}
}
