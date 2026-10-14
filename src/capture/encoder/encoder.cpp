#include "encoder.hpp"
#include "resource_layout.hpp"
#include "query_dependencies.hpp"
#include "msaa_samples.hpp"
#include "capture_msaa_shaders.hpp"
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
    std::map<ObjectId,MsaaInitializationLayout> msaa_;
    std::set<ObjectId> initialized_counters_;
    std::array<bool,size_t(ShaderStage::count)> constant_windows_{};
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
    void preflight_msaa(){
        size_t bytes=0,planes=0;
        for(const auto& [id,samples]:frame_.initial_msaa){
            checkpoint();require(id && id==samples.resource,"Encoder MSAA key/resource identity mismatch");
            require(!frame_.initial_resources.contains(id),"Encoder resource has both ordinary and MSAA contents");
            require(id!=frame_.output,"Encoder MSAA output profile unsupported");
            const auto found=frame_.objects.find(id);require(found!=frame_.objects.end(),"Encoder orphan MSAA initial contents");
            const auto& o=found->second;require(o.kind=="texture2d" || o.kind=="texture2d1","Encoder MSAA object kind mismatch");
            require(o.descriptor.size()==(o.kind=="texture2d"?sizeof(D3D11_TEXTURE2D_DESC):sizeof(D3D11_TEXTURE2D_DESC1)),"Encoder MSAA descriptor length mismatch");
            if(o.kind=="texture2d1")require(descriptor<D3D11_TEXTURE2D_DESC1>(o).TextureLayout==D3D11_TEXTURE_LAYOUT_UNDEFINED,"Encoder explicit MSAA texture layout unsupported");
            auto plan=validate_msaa_initialization(samples,descriptor<D3D11_TEXTURE2D_DESC>(o),options_.msaa_byte_limit-bytes,options_.msaa_plane_limit-planes);
            bytes+=plan.bytes;planes+=plan.planes;msaa_.emplace(id,plan);
        }
        if(msaa_.empty())return;
        for(const auto& command:frame_.commands){
            // EVENT observations still pass the shared provenance/scope gate in write().
            // MSAA helper work precedes application commands; it does not establish
            // completion of any captured in-frame End.
            checkpoint();
            require(!command.bindings.predicate,"Encoder predicated MSAA frame is not verified");
            if(command.kind==CommandKind::copy_resource || command.kind==CommandKind::copy_region ||
               command.kind==CommandKind::update_subresource || command.kind==CommandKind::map || command.kind==CommandKind::unmap)
                require(!msaa_.contains(command.operand) && !msaa_.contains(command.source),"Encoder MSAA mutation command is not verified");
        }
    }
    template<class T>uint64_t helper_object(uint16_t type,const T& desc){
        const auto id=allocate();add(id,5,type,Payload{}.q({id,2}).raw(desc));return id;
    }
    uint64_t helper_texture(const D3D11_TEXTURE2D_DESC& d,std::span<const uint8_t> data){
        const auto id=allocate();add(id,5,0x85,Payload{}.q({id,2}).raw(d).value(blob(data)));return id;
    }
    template<class T>uint64_t helper_view(uint16_t type,uint64_t resource,const T& d){
        const auto id=allocate();add(id,5,type,Payload{}.q({id,2,resource}).raw(d));return id;
    }
    uint64_t helper_shader(uint16_t type,std::span<const uint8_t> code){
        const auto data=add(9,0x81,Payload{}.value(uint64_t(code.size())).bytes(code).q({0}));
        const auto id=allocate();add(id,5,type,Payload{}.q({id,2,0,0,0,0,data}));return id;
    }
    uint64_t initialize_msaa_depth(const MsaaSamples& samples,uint64_t vertex,uint64_t raster,uint64_t scissors){
        const auto& shape=samples.shape;
        const auto depth_pixel=helper_shader(0x92,msaa_shaders::depth),stencil_pixel=helper_shader(0x92,msaa_shaders::stencil);
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
        ds.StencilReadMask=255;ds.StencilWriteMask=255;ds.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS};ds.BackFace=ds.FrontFace;
        const auto depth_state=helper_object(0x8b,ds);
        std::array<uint64_t,8> stencil_states{},constants{};
        for(UINT bit=0;bit<8;++bit){
            ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.StencilEnable=TRUE;ds.StencilWriteMask=uint8_t(1u<<bit);stencil_states[bit]=helper_object(0x8b,ds);
            D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
            const auto data=blob(Payload{}.u({1u<<bit,0,0,0})),id=allocate();add(id,5,0x83,Payload{}.q({id,2}).raw(bd).value(data));constants[bit]=id;
        }
        D3D11_TEXTURE2D_DESC td{shape.width,shape.height,1,shape.array_size,DXGI_FORMAT_R32G8X24_TYPELESS,{shape.samples,samples.quality},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL,0,0};
        const auto temporary=helper_texture(td,{});std::vector<uint64_t> views;
        for(UINT layer=0;layer<shape.array_size;++layer){D3D11_DEPTH_STENCIL_VIEW_DESC d{};d.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray={layer,1};
            const auto view=helper_view(0x8e,temporary,d);views.push_back(view);add(7,0x31,Payload{}.q({0,1,view}).u({3}).value(0.0f).value(uint8_t(0)));}
        const D3D11_VIEWPORT viewport{0,0,float(shape.width),float(shape.height),0,1};const auto viewports=add(9,0x87,Payload{}.u({1}).raw(viewport));
        for(const auto& plane:samples.planes){
            checkpoint();D3D11_TEXTURE2D_DESC d{shape.width,shape.height,1,1,DXGI_FORMAT_R32G32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
            const auto texture=helper_texture(d,plane.bytes);D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=d.Format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
            const auto srv=helper_view(0x8c,texture,sd);state_block state{};state.mask.fill(UINT32_MAX);
            state.topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;state.vs_shader=vertex;state.ps_shader=depth_pixel;state.ps_srv[0]=srv;state.rasterizer=raster;
            state.viewports=viewports;state.scissors=scissors;state.sample_mask=1u<<plane.sample;state.dsv=views.at(plane.array_slice);state.depth_state=depth_state;state.om_uav_start=64;
            state.om_uav_counts.fill(UINT32_MAX);state.cs_uav_counts.fill(UINT32_MAX);
            auto draw=[&]{const auto id=add(3,3,Payload{}.bytes(encode_state_block(state)));add(7,0x37,Payload{}.q({id,0,1}).u({3,0}));};draw();
            state.ps_shader=stencil_pixel;
            for(UINT bit=0;bit<8;++bit){state.depth_state=stencil_states[bit];state.stencil_ref=1u<<bit;state.ps_cb[0]=constants[bit];draw();}
        }
        return temporary;
    }
    void initialize_msaa(){
        if(msaa_.empty())return;
        const auto vertex=helper_shader(0x90,msaa_shaders::vertex);
        const bool color=std::any_of(msaa_.begin(),msaa_.end(),[](const auto& entry){return entry.second.access.encoding==MsaaSampleEncoding::color_bits;});
        const auto pixel=color?helper_shader(0x92,msaa_shaders::color):0;
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;
        const auto raster=helper_object(0x89,rd),scissors=add(9,0x86,Payload{}.u({0}));
        for(const auto& [id,plan]:msaa_){
            checkpoint();const auto& samples=frame_.initial_msaa.at(id);const auto& shape=samples.shape;
            std::u16string name=u"FloraCapture initialization: MSAA resource ";for(char c:std::to_string(id))name.push_back(char16_t(c));name.push_back(0);
            add(7,0x327b,Payload{}.q({0,UINT64_MAX-2}).u({0,narrow(name.size()*2)}).bytes({reinterpret_cast<const uint8_t*>(name.data()),name.size()*2}));
            if(plan.access.encoding==MsaaSampleEncoding::depth32_stencil8){
                const auto temporary=initialize_msaa_depth(samples,vertex,raster,scissors);
                add(7,0x3e,Payload{}.q({0,1,resource_ref(id),temporary}));add(7,0x327c,Payload{}.q({0,UINT64_MAX-2}).u({0}));continue;
            }
            D3D11_TEXTURE2D_DESC td{shape.width,shape.height,1,shape.array_size,plan.access.storage,{shape.samples,samples.quality},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
            const auto temporary=helper_texture(td,{});
            std::vector<uint64_t> views;
            for(UINT layer=0;layer<shape.array_size;++layer){D3D11_RENDER_TARGET_VIEW_DESC d{};d.Format=plan.access.view;d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray={layer,1};
                const auto view=helper_view(0x8d,temporary,d);views.push_back(view);add(7,0x32,Payload{}.q({0,1,view}).value<uint8_t>(1).u({0,0,0,0}));}
            const D3D11_VIEWPORT viewport{0,0,float(shape.width),float(shape.height),0,1};
            const auto viewports=add(9,0x87,Payload{}.u({1}).raw(viewport));
            for(const auto& plane:samples.planes){
                checkpoint();D3D11_TEXTURE2D_DESC d{shape.width,shape.height,1,1,plan.access.view,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
                const auto texture=helper_texture(d,plane.bytes);D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=d.Format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
                const auto srv=helper_view(0x8c,texture,sd);state_block state{};state.mask.fill(UINT32_MAX);
                state.topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;state.vs_shader=vertex;state.ps_shader=pixel;state.ps_srv[0]=srv;state.rasterizer=raster;
                state.viewports=viewports;state.scissors=scissors;state.sample_mask=1u<<plane.sample;state.rtv[0]=views.at(plane.array_slice);state.rt_count=1;state.om_uav_start=64;
                state.om_uav_counts.fill(UINT32_MAX);state.cs_uav_counts.fill(UINT32_MAX);
                const auto state_id=add(3,3,Payload{}.bytes(encode_state_block(state)));
                add(7,0x37,Payload{}.q({state_id,0,1}).u({3,0}));
            }
            add(7,0x3e,Payload{}.q({0,1,resource_ref(id),temporary}));add(7,0x327c,Payload{}.q({0,UINT64_MAX-2}).u({0}));
        }
    }
    void resource(ObjectId id,const FrozenObject& object){
        if(msaa_.contains(id)){
            const auto d=descriptor<D3D11_TEXTURE2D_DESC>(object);
            // Ordinary GenData cannot represent MSAA. The explicit helper
            // sequence below supplies every sample before application commands.
            add(ref(id),5,0x85,Payload{}.q({ref(id),2}).raw(d).value(blob({})));return;
        }
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
    void validate_msaa_view(const FrozenObject& o){
        const auto& shape=frame_.initial_msaa.at(o.resource).shape;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;UINT first=0,count=0;
        if(o.kind=="srv" || o.kind=="srv1"){
            const auto d=descriptor<D3D11_SHADER_RESOURCE_VIEW_DESC>(o);format=d.Format;
            if(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DMS){require(shape.array_size==1,"Encoder MSAA non-array SRV targets an array");count=1;}
            else{require(d.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY,"Encoder MSAA SRV dimension mismatch");first=d.Texture2DMSArray.FirstArraySlice;count=d.Texture2DMSArray.ArraySize;}
        }else if(o.kind=="rtv" || o.kind=="rtv1"){
            const auto d=descriptor<D3D11_RENDER_TARGET_VIEW_DESC>(o);format=d.Format;
            if(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DMS){require(shape.array_size==1,"Encoder MSAA non-array RTV targets an array");count=1;}
            else{require(d.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY,"Encoder MSAA RTV dimension mismatch");first=d.Texture2DMSArray.FirstArraySlice;count=d.Texture2DMSArray.ArraySize;}
        }else if(o.kind=="dsv"){
            const auto d=descriptor<D3D11_DEPTH_STENCIL_VIEW_DESC>(o);format=d.Format;
            require(d.Flags==0,"Encoder MSAA read-only DSV profile is not verified");
            if(d.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DMS){require(shape.array_size==1,"Encoder MSAA non-array DSV targets an array");count=1;}
            else{require(d.ViewDimension==D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY,"Encoder MSAA DSV dimension mismatch");first=d.Texture2DMSArray.FirstArraySlice;count=d.Texture2DMSArray.ArraySize;}
        }else throw std::runtime_error("Encoder MSAA view kind unsupported");
        require(first<shape.array_size && count && count<=shape.array_size-first,"Encoder MSAA view array range invalid");
        const auto storage=msaa_.at(o.resource).access.storage;
        if(msaa_.at(o.resource).access.encoding==MsaaSampleEncoding::depth32_stencil8){
            const bool dsv=o.kind=="dsv",srv=o.kind=="srv" || o.kind=="srv1";
            require(dsv?format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
                (srv && shape.format==storage && (format==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || format==DXGI_FORMAT_X32_TYPELESS_G8X24_UINT)),"Encoder MSAA depth view format incompatible");
        }else require(o.kind!="dsv" && msaa_initialization_color_format(format) && format!=storage &&
            msaa_integer_access(format).storage==storage &&
            (shape.format==storage || shape.format==format),"Encoder MSAA view format incompatible");
        const auto resource=descriptor<D3D11_TEXTURE2D_DESC>(frame_.objects.at(o.resource));
        const UINT required=(o.kind=="srv" || o.kind=="srv1")?D3D11_BIND_SHADER_RESOURCE:o.kind=="dsv"?D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET;
        require((resource.BindFlags&required)!=0,"Encoder MSAA view requires missing bind flag");
    }
    void object(ObjectId id,const FrozenObject& o){
        if(id==device_ || id==context_)return;
        require(o.device==device_,"Encoder object belongs to another device");
        if(o.kind=="query" || o.kind=="query1"){
            const auto d=descriptor<D3D11_QUERY_DESC>(o);
            require(d.Query==D3D11_QUERY_EVENT && !d.MiscFlags,"Encoder ordinary query profile unsupported");
            if(o.kind=="query1")require(descriptor<D3D11_QUERY_DESC1>(o).ContextType==D3D11_CONTEXT_TYPE_ALL,"Encoder Query1 context profile unsupported");
            // Descriptor observation identifies an ordinary query without inventing
            // a GPA predicate resource. Query identity is reserved but not materialized.
            add(7,0x3152,Payload{}.q({0,ref(id)}).value<uint8_t>(1).u({0,0}));return;
        }
        if(o.kind=="buffer" || o.kind=="texture1d" || o.kind=="texture2d" || o.kind=="texture2d1" || o.kind=="texture3d" || o.kind=="texture3d1"){resource(id,o);return;}
        Payload p;p.q({ref(id),2});uint16_t type=0;
        if(o.resource){
            require(frame_.initial_resources.contains(o.resource) || msaa_.contains(o.resource),"Encoder view source has no resource data");p.value(resource_ref(o.resource));
            if(msaa_.contains(o.resource))validate_msaa_view(o);
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
    void constant_buffers(const PipelineBindings& bindings,size_t index){
        const auto& stage=bindings.shaders[index];bool window=false;
        for(size_t slot=0;slot<stage.buffers.size();++slot){
            if(!stage.buffers[slot])continue;
            ref(stage.buffers[slot],"buffer");const auto d=descriptor<D3D11_BUFFER_DESC>(frame_.objects.at(stage.buffers[slot]));
            require(d.BindFlags==D3D11_BIND_CONSTANT_BUFFER && d.ByteWidth && !(d.ByteWidth%16),"Encoder invalid constant-buffer descriptor");
            const auto first=stage.first_constants[slot],count=stage.constant_counts[slot];
            if(!bindings.constant_ranges){require(!first,"Encoder constant-buffer window observation missing");continue;}
            require(!(first%16) && !(count%16) && count<=4096,"Encoder invalid constant-buffer window");
            window=window || first || count!=4096;
        }
        if(!window && !constant_windows_[index])return;
        // State blocks contain buffer identities, but no CB1 windows. Emit the
        // complete observed stage immediately before its Draw/Dispatch, and an
        // ordinary setter when returning to whole buffers to end prior windows.
        Payload p;p.q({0,1}).u({0,narrow(stage.buffers.size())}).value<uint8_t>(1);
        for(auto id:stage.buffers)p.value(ref(id,"buffer"));
        if(window){
            p.value<uint8_t>(1);for(size_t slot=0;slot<stage.buffers.size();++slot)p.value(stage.buffers[slot]?stage.first_constants[slot]:0u);
            p.value<uint8_t>(1);for(size_t slot=0;slot<stage.buffers.size();++slot)p.value(stage.buffers[slot]?stage.constant_counts[slot]:0u);
        }
        add(7,uint16_t((window?0x24f:0x249)+index),std::move(p));constant_windows_[index]=window;
    }
    void counter_resets(const RecordedCommand& command,bool unbound=false){
        const auto& s=command.bindings;
        const bool om_reset=unbound || std::any_of(command.uav_counters.begin(),command.uav_counters.end(),[&](const auto& entry){
            return std::find(s.om_uavs.begin(),s.om_uavs.end(),entry.first)!=s.om_uavs.end();});
        if(om_reset){
            std::u16string name=unbound?u"FloraCapture counter reset: copy source":u"FloraCapture counter reset: OM through CS";name.push_back(0);
            add(7,0x327b,Payload{}.q({0,UINT64_MAX-3}).u({0,narrow(name.size()*2)}).bytes({reinterpret_cast<const uint8_t*>(name.data()),name.size()*2}));
            // Avoid output/input hazards while temporarily binding the same view
            // to CS. The following complete action snapshot restores bindings;
            // CB1 setters must be emitted AFTER this helper, not cleared by it.
            add(7,0x242,Payload{}.q({0,1}));
        }
        for(const auto& [id,count]:command.uav_counters){
            const auto cs=std::find(s.cs_uavs.begin(),s.cs_uavs.end(),id);
            const auto om=std::find(s.om_uavs.begin(),s.om_uavs.end(),id);
            require(unbound || cs!=s.cs_uavs.end() || om!=s.om_uavs.end(),"Encoder counter reset is not bound");
            const bool temporary=unbound || cs==s.cs_uavs.end();
            Payload p;p.q({0,1}).u({temporary?0u:UINT(cs-s.cs_uavs.begin()),1})
                .value<uint8_t>(1).value(typed(id,{"uav","uav1"})).value<uint8_t>(1).value(count);
            add(7,0x25e,std::move(p));
            if(temporary)add(7,0x25e,Payload{}.q({0,1}).u({0,1}).value<uint8_t>(1).value<uint64_t>(0).value<uint8_t>(0));
        }
        if(om_reset)add(7,0x327c,Payload{}.q({0,UINT64_MAX-3}).u({0}));
    }
    uint64_t state(const RecordedCommand& command){
        const auto& s=command.bindings;require(s.context==context_ && s.device==device_,"Encoder mixed context/device state");
        require(!s.predicate && std::all_of(s.stream_targets.begin(),s.stream_targets.end(),[](auto id){return id==0;}),"Predicate/SO state encoding pending");
        for(const auto& [id,lod]:s.resource_min_lod){resource_ref(id);require(lod==0,"Nonzero resource minimum LOD encoding pending");}
        counter_resets(command);
        state_block out{};out.mask.fill(UINT32_MAX);
        out.ib=ref(s.index_buffer,"buffer");out.ib_format=s.index_format;out.ib_offset=s.index_offset;out.input_layout=ref(s.input_layout,"input_layout");out.topology=s.topology;
        refs(s.vertices,out.vb,{"buffer"});out.strides=s.strides;out.offsets=s.offsets;
#define STAGE(NAME,INDEX) {const auto& stage=s.shaders[size_t(ShaderStage::INDEX)];require(stage.classes.empty(),"Dynamic class state encoding pending"); \
    constant_buffers(s,size_t(ShaderStage::INDEX)); \
    refs(stage.buffers,out.NAME##_cb,{"buffer"});refs(stage.samplers,out.NAME##_samplers,{"sampler"});refs(stage.resources,out.NAME##_srv,{"srv","srv1"});out.NAME##_shader=ref(stage.shader,#INDEX "_shader");}
        STAGE(vs,vertex) STAGE(hs,hull) STAGE(ds,domain) STAGE(gs,geometry) STAGE(ps,pixel) STAGE(cs,compute)
#undef STAGE
        require(s.viewports.size()<=16 && s.scissors.size()<=16,"Encoder viewport/scissor limit exceeded");
        Payload viewports;viewports.value(narrow(s.viewports.size()));for(const auto& v:s.viewports)viewports.raw(v);out.viewports=add(9,0x87,std::move(viewports));
        // Always materialize the array, including count zero. Mask bit 672 must
        // stay enabled so the original player applies changes and empty resets.
        Payload scissors;scissors.value(narrow(s.scissors.size()));for(const auto& v:s.scissors)scissors.raw(v);out.scissors=add(9,0x86,std::move(scissors));
        out.rasterizer=typed(s.rasterizer,{"rasterizer","rasterizer1","rasterizer2"});out.blend=typed(s.blend,{"blend","blend1"});out.blend_factor=s.blend_factor;out.sample_mask=s.sample_mask;out.depth_state=ref(s.depth_stencil,"depth_stencil");out.stencil_ref=s.stencil_ref;out.dsv=ref(s.depth_view,"dsv");
        for(const auto& [id,count]:command.uav_counters){require(id!=0,"Encoder counter identity is null");typed(id,{"uav","uav1"});require(count!=UINT_MAX && !command.uav_counter_keeps.contains(id),"Encoder counter reset/KEEP is ambiguous");}
        for(auto id:command.uav_counter_keeps){require(id && (std::find(s.cs_uavs.begin(),s.cs_uavs.end(),id)!=s.cs_uavs.end() || std::find(s.om_uavs.begin(),s.om_uavs.end(),id)!=s.om_uavs.end()),"Encoder counter KEEP is not bound");
            typed(id,{"uav","uav1"});const auto d=descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC>(frame_.objects.at(id));
            require(d.ViewDimension==D3D11_UAV_DIMENSION_BUFFER && (d.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER)),"Encoder KEEP view has no hidden counter");}
        auto require_counter=[&](ObjectId id){if(!id)return;typed(id,{"uav","uav1"});const auto d=descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC>(frame_.objects.at(id));
            if(d.ViewDimension==D3D11_UAV_DIMENSION_BUFFER && (d.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER)))
                {if(command.uav_counters.contains(id))initialized_counters_.insert(id);
                 else require(command.uav_counter_keeps.contains(id) && initialized_counters_.contains(id),"Encoder hidden UAV counter is missing");}};
        out.om_uav_start=64;out.om_uav_counts.fill(UINT32_MAX);out.cs_uav_counts.fill(UINT32_MAX);
        for(size_t i=0;i<64;++i){require(i<8 || (!s.om_uavs[i] && !s.cs_uavs[i]),"Extended UAV wire validation pending");if(i>=8)continue;
            require_counter(s.om_uavs[i]);require_counter(s.cs_uavs[i]);
            out.rtv[i]=typed(s.render_targets[i],{"rtv","rtv1"});if(out.rtv[i])out.rt_count=UINT(i+1);
            if(s.om_uavs[i]){require(!s.render_targets[i],"Overlapping RTV/UAV state");out.rtv[i]=typed(s.om_uavs[i],{"uav","uav1"});out.om_uav_start=(std::min)(out.om_uav_start,UINT(i));out.rt_count=UINT(i+1);}
            out.cs_uav[i]=typed(s.cs_uavs[i],{"uav","uav1"});if(out.cs_uav[i])out.cs_uav_count=UINT(i+1);
        }
        // Setters establish initialization; snapshots KEEP the GPU-updated value.
        return add(3,3,Payload{}.bytes(encode_state_block(out)));
    }
public:
    Encoder(const FrozenFrame& frame,const EncodeOptions& options):frame_(frame),options_(options){}
    void write(const std::filesystem::path& path){
        checkpoint();
        preflight_msaa();
        for(const auto& command:frame_.commands)
            if(command.kind==CommandKind::query_end || command.kind==CommandKind::query_get_data){
                EventDependencyOptions analysis_options;analysis_options.cancelled=options_.cancelled;
                const auto dependencies=analyze_event_dependencies(frame_,analysis_options);
                bool observations_only=frame_.start_completion.has_value();
                for(const auto& observed:dependencies.observations){
                    const auto& c=frame_.commands.at(observed.sequence);const auto& q=*c.query;
                    const bool pending=observed.scope==EventObservationScope::pending && q.end_sequence &&
                        q.last_end.immediate && q.last_end.context==c.bindings.context &&
                        !q.has_output && c.words[0]==0;
                    observations_only=observations_only && (observed.scope==EventObservationScope::preframe_completed || pending);
                }
                // No in-frame successful completion is lowered here. Pre-frame
                // work was drained before the initial state; a pending observation
                // and an End alone promise no GPU completion during this frame.
                if(observations_only)break;
                std::string detail="; End observations="+std::to_string(dependencies.ends)+", GetData observations="+std::to_string(dependencies.observations.size());
                std::array<size_t,8> scopes{};
                const EventDependencyObservation* latest=nullptr;
                for(const auto& observed:dependencies.observations){
                    ++scopes.at(size_t(observed.scope));
                    if(observed.prefix && (!latest || observed.end_sequence>latest->end_sequence))latest=&observed;
                }
                constexpr const char* scope_names[]{"pending","failed","unissued","pre-frame","deferred","foreign-context","recorded-prefix","pre-frame-completed"};
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
        initialize_msaa();
        std::map<std::pair<ObjectId,UINT>,uint64_t> mappings;
        for(const auto& initial:frame_.initial_maps){
            checkpoint();
            require(initial.resource && initial.context==context_ && initial.device==device_ && initial.generation && initial.thread,
                "Encoder boundary Map provenance mismatch");
            require(frame_.start_completion && initial.thread==frame_.start_completion->thread,"Encoder boundary Map thread mismatch");
            const auto resource=typed(initial.resource,{"buffer"});const auto& object=frame_.objects.at(initial.resource);
            require(object.device==device_ && object.descriptor.size()==sizeof(D3D11_BUFFER_DESC),"Encoder boundary Map resource mismatch");
            const auto d=descriptor<D3D11_BUFFER_DESC>(object);
            require(d.Usage==D3D11_USAGE_DYNAMIC && d.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE && !d.MiscFlags && !d.StructureByteStride && d.BindFlags &&
                !(d.BindFlags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE)),"Encoder boundary Map storage unsupported");
            require(frame_.initial_resources.contains(initial.resource),"Encoder boundary Map initial data missing");
            const auto& baseline=frame_.initial_resources.at(initial.resource);
            require(baseline.shape.dimension==D3D11_RESOURCE_DIMENSION_BUFFER && baseline.shape.width==d.ByteWidth && initial.data.size()==d.ByteWidth,
                "Encoder boundary Map byte length mismatch");
            require(mappings.emplace(std::pair{initial.resource,0u},UINT64_MAX).second,"Encoder duplicate boundary Map");
            std::u16string name=u"FloraCapture initialization: frame-start Map";name.push_back(0);
            add(7,0x327b,Payload{}.q({0,UINT64_MAX-2}).u({0,narrow(name.size()*2)}).bytes({reinterpret_cast<const uint8_t*>(name.data()),name.size()*2}));
            add(7,0x246,Payload{}.q({0,1}).value<int32_t>(S_OK).value(resource).u({0,D3D11_MAP_WRITE_DISCARD,0}).value(blob(initial.data)));
            add(7,0x327c,Payload{}.q({0,UINT64_MAX-2}).u({0}));
        }
        uint64_t sequence=0;
        for(const auto& command:frame_.commands){
            require(command.sequence==sequence++,"Encoder command sequence mismatch");const auto& w=command.words;Payload p;uint16_t type=0;
            const bool gpu_action=is_gpu_action(command.kind);
            require(gpu_action || command.kind==CommandKind::copy_structure_count || command.uav_counter_keeps.empty(),"Encoder counter KEEP attached to non-GPU command");
            require(command.kind==CommandKind::update_subresource || !command.update_box,"Encoder box attached to non-Update command");
            require(command.kind==CommandKind::copy_region || !command.copy_region,"Encoder region attached to non-copy command");
            if(command.kind==CommandKind::query_end || command.kind==CommandKind::query_get_data){
                require(command.query.has_value() && command.completed && command.bindings.context==context_ && command.bindings.device==device_,"Encoder query observation mismatch");
                const auto query=typed(command.operand,{"query","query1"});
                if(command.kind==CommandKind::query_end)add(7,0x34fa,Payload{}.q({0,1,query}));
                else{const auto& q=*command.query;p.q({0,1}).value(command.result).value(query).value<uint8_t>(q.has_output?1:0);
                    if(q.has_output){require(q.value.has_value(),"Encoder query output bytes unavailable");p.value(*q.value);}p.u({w[0],w[1]});add(7,0x34fb,std::move(p));}
                continue;
            }
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
                    require(mappings.emplace(key,command.sequence).second,"Encoder overlapping Map calls");
                    if(w[1]==D3D11_MAP_WRITE_NO_OVERWRITE)data=mapped_diff(command.before_data,command.data);
                    else{require(command.before_data.empty(),"Encoder unexpected Map baseline");data=blob(command.data);}
                }
                add(7,w[1]==D3D11_MAP_READ?0x34ec:0x246,Payload{}.q({0,1}).value(command.result).value(resource).u({w[0],w[1],w[2]}).value(data));continue;
            }
            if(!mappings.empty()){
                auto unmapped=[&](ObjectId id){
                    if(!id)return;
                    const auto found=frame_.objects.find(id);require(found!=frame_.objects.end(),"Encoder Map dependency definition missing");
                    const auto resource=found->second.resource?found->second.resource:id;
                    for(const auto& [mapped,unused]:mappings){(void)unused;require(mapped.first!=resource,"Encoder GPU dependency has open Map");}
                };
                unmapped(command.operand);unmapped(command.source);
                const auto& state=command.bindings;
                for(const auto& stage:state.shaders){for(auto id:stage.buffers)unmapped(id);for(auto id:stage.resources)unmapped(id);}
                for(auto id:state.vertices)unmapped(id);for(auto id:state.render_targets)unmapped(id);
                for(auto id:state.stream_targets)unmapped(id);for(auto id:state.om_uavs)unmapped(id);for(auto id:state.cs_uavs)unmapped(id);
                unmapped(state.index_buffer);unmapped(state.depth_view);
            }
            if(command.kind==CommandKind::update_subresource){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder Update context mismatch");
                require(command.completed && command.operand && w[3]<=1 && !w[4],"Encoder incomplete or invalid Update");
                require(!command.bindings.predicate,"Encoder predicated Update is unsupported");
                const auto destination=resource_ref(command.operand);const auto& resource=frame_.initial_resources.at(command.operand);
                const auto layout=update_layout(resource.shape,frame_.objects.at(command.operand).descriptor,w[0],command.update_box?&*command.update_box:nullptr);
                require(command.data.size()==layout.slice_bytes*layout.depth && command.before_data.empty(),"Encoder Update payload size mismatch");
                require(layout.rows<=1 || w[1]>=layout.row_bytes,"Encoder Update row pitch too small");
                require(layout.depth<=1 || uint64_t(w[2])>=uint64_t(layout.rows-1)*w[1]+layout.row_bytes,"Encoder Update depth pitch too small");
                const auto data=blob(command.data);Payload payload;
                payload.q({0,1,destination}).value(w[0]).value<uint8_t>(command.update_box?1:0);
                if(command.update_box)for(auto coordinate:*command.update_box)payload.value(coordinate);
                payload.value(data).u({w[1],w[2]});
                if(w[3])payload.value(w[4]);add(7,w[3]?0x255:0x247,std::move(payload));continue;
            }
            if(command.kind==CommandKind::copy_structure_count){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder counter copy context mismatch");
                require(command.completed && command.operand && command.source && !w[1] && !w[2] && !w[3] && !w[4],"Encoder counter copy arguments incomplete");
                require(!command.bindings.predicate && command.data.empty() && command.before_data.empty(),"Encoder counter copy has predicate/CPU bytes");
                const auto destination=ref(command.operand,"buffer"),source=typed(command.source,{"uav","uav1"});
                const auto& view=frame_.objects.at(command.source);ref(view.resource,"buffer");
                require(view.resource!=command.operand && frame_.initial_resources.contains(command.operand) && frame_.initial_resources.contains(view.resource),"Encoder counter copy resource data missing/aliased");
                validate_copy_structure_count(descriptor<D3D11_BUFFER_DESC>(frame_.objects.at(command.operand)),descriptor<D3D11_UNORDERED_ACCESS_VIEW_DESC>(view),descriptor<D3D11_BUFFER_DESC>(frame_.objects.at(view.resource)),w[0]);
                require(command.uav_counters.size()+command.uav_counter_keeps.size()==1,"Encoder counter copy hidden state missing/extra");
                if(command.uav_counters.contains(command.source)){
                    require(command.uav_counters.at(command.source)!=UINT_MAX,"Encoder counter copy numeric UINT_MAX unsupported");
                    counter_resets(command,true);initialized_counters_.insert(command.source);
                }else require(command.uav_counter_keeps.contains(command.source) && initialized_counters_.contains(command.source),"Encoder counter copy KEEP has no initial value");
                add(7,0x3f,Payload{}.q({0,1,destination}).value(w[0]).value(source));continue;
            }
            if(command.kind==CommandKind::copy_region){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder region copy context mismatch");
                require(!command.bindings.predicate,"Encoder predicated region copy is unsupported");
                require(command.completed && command.copy_region && command.operand && command.source,"Encoder region copy data missing");
                require(command.data.empty() && command.before_data.empty(),"Encoder region copy has unexpected CPU bytes");
                const auto destination=resource_ref(command.operand),source=resource_ref(command.source);
                const auto& d=frame_.initial_resources.at(command.operand);const auto& s=frame_.initial_resources.at(command.source);const auto& c=*command.copy_region;
                validate_copy_region(d.shape,frame_.objects.at(command.operand).descriptor,s.shape,frame_.objects.at(command.source).descriptor,c,command.operand==command.source);
                p.q({0,1,destination}).u({c.destination_subresource,c.x,c.y,c.z}).value(source).value(c.source_subresource).value<uint8_t>(c.box?1:0);
                if(c.box)for(auto v:*c.box)p.value(v);
                if(c.extended)p.value(c.flags);
                add(7,c.extended?0x256:0x40,std::move(p));continue;
            }
            if(command.kind==CommandKind::resolve){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder Resolve context mismatch");
                require(command.completed && command.operand && command.source && !w[3] && !w[4],"Encoder Resolve arguments incomplete or unsupported");
                require(!command.bindings.predicate && command.data.empty() && command.before_data.empty(),"Encoder Resolve has predicate or unexpected CPU bytes");
                require(frame_.initial_resources.contains(command.operand) && msaa_.contains(command.source),"Encoder Resolve initial resources missing");
                const auto& d=frame_.initial_resources.at(command.operand);const auto& s=frame_.initial_msaa.at(command.source);
                validate_resolve(d.shape,frame_.objects.at(command.operand).descriptor,s.shape,frame_.objects.at(command.source).descriptor,
                    w[0],w[1],DXGI_FORMAT(w[2]),command.operand==command.source);
                add(7,0x42,Payload{}.q({0,1,resource_ref(command.operand)}).value(w[0]).value(resource_ref(command.source)).u({w[1],w[2]}));continue;
            }
            if(command.kind==CommandKind::copy_resource){
                require(command.bindings.context==context_ && command.bindings.device==device_,"Encoder copy context mismatch");
                require(!command.bindings.predicate,"Encoder predicated copy is unsupported");
                require(command.operand && command.source && command.operand!=command.source,"Encoder copy operands are missing or alias");
                const auto destination=resource_ref(command.operand),source=resource_ref(command.source);
                const auto& d=frame_.initial_resources.at(command.operand);
                validate_copy_resource(d.shape,frame_.objects.at(command.operand).descriptor,frame_.initial_resources.at(command.source).shape);
                add(7,0x3e,Payload{}.q({0,1,destination,source}));continue;
            }
            if(indirect_argument_bytes(command.kind)){
                require(command.operand && !command.source && !w[1] && !w[2] && !w[3] && !w[4] && command.data.empty() && command.before_data.empty(),"Encoder indirect arguments invalid");
                const auto arguments=ref(command.operand,"buffer");require(frame_.initial_resources.contains(command.operand),"Encoder indirect initial data missing");
                validate_indirect_arguments(descriptor<D3D11_BUFFER_DESC>(frame_.objects.at(command.operand)),command.kind,w[0]);
                const auto indirect_type=command.kind==CommandKind::dispatch_indirect?0x36:command.kind==CommandKind::draw_instanced_indirect?0x3d:0x3b;
                add(7,uint16_t(indirect_type),Payload{}.q({state(command),0,1,arguments}).value(w[0]));continue;
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
        for(const auto& final_map:frame_.final_maps){
            require(final_map.resource && final_map.context==context_ && final_map.device==device_ && final_map.generation && final_map.thread && frame_.start_completion &&
                final_map.thread==frame_.start_completion->thread && final_map.command_sequence<frame_.commands.size(),"Encoder final Map provenance mismatch");
            const auto& command=frame_.commands.at(final_map.command_sequence);
            require(command.kind==CommandKind::map && command.operand==final_map.resource && command.words==std::array<uint32_t,5>{0,D3D11_MAP_WRITE_DISCARD,0,0,0} &&
                command.result==S_OK && command.completed,"Encoder final Map command mismatch");
            const auto resource=typed(final_map.resource,{"buffer"});const auto& object=frame_.objects.at(final_map.resource);
            require(object.device==device_ && object.descriptor.size()==sizeof(D3D11_BUFFER_DESC),"Encoder final Map resource mismatch");
            const auto d=descriptor<D3D11_BUFFER_DESC>(object);
            require(d.Usage==D3D11_USAGE_DYNAMIC && d.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE && !d.MiscFlags && !d.StructureByteStride && d.BindFlags &&
                !(d.BindFlags&~(D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE)),"Encoder final Map storage unsupported");
            const auto open=mappings.find({final_map.resource,0});require(open!=mappings.end() && open->second==final_map.command_sequence,"Encoder final Map is not the open generation or is duplicated");mappings.erase(open);
            std::u16string name=u"FloraCapture cleanup: frame-end Map";name.push_back(0);
            add(7,0x327b,Payload{}.q({0,UINT64_MAX-2}).u({0,narrow(name.size()*2)}).bytes({reinterpret_cast<const uint8_t*>(name.data()),name.size()*2}));
            add(7,0x34ed,Payload{}.q({0,1,resource}).u({0}));
            add(7,0x327c,Payload{}.q({0,UINT64_MAX-2}).u({0}));
        }
        require(mappings.empty(),"Encoder frame has unpaired Map");
        const auto reference_data=blob(final);add(5,0x87,Payload{}.q({UINT64_MAX-1,3}).raw(output_desc).value(reference_data));
        add(7,0x3257,Payload{}.q({0,3}).u({0,frame_.presentation?frame_.presentation->sync_interval:1,frame_.presentation?frame_.presentation->flags:0}));
        Header header{};memcpy(header.data(),"IGPA",4);wire_write<uint32_t>(header.data()+4,HEADER_SIZE);wire_write<uint32_t>(header.data()+8,VERSION);
        GUID guid{};require(SUCCEEDED(CoCreateGuid(&guid)),"Frame GUID generation failed");memcpy(header.data()+0x20,&guid,16);memcpy(header.data()+API_OFFSET,"DX11",4);memcpy(header.data()+0x4c,"DX11_0",6);
        require(frame_.producer.size()<32 && frame_.producer.find('\0')==std::string::npos,"Encoder producer name is invalid");memcpy(header.data()+0xd4,frame_.producer.data(),frame_.producer.size());wire_write<uint32_t>(header.data()+0xfc,6);
        wire_write(header.data()+DIMENSIONS_OFFSET,shape.width);wire_write(header.data()+DIMENSIONS_OFFSET+4,shape.height);
        // GPA 2025 R1's capture browser treats a negative BMP height as an
        // unsigned allocation size. Use bottom-up rows; GPU records stay intact.
        ByteBuffer bmp(54+final.size());bmp[0]='B';bmp[1]='M';wire_write(bmp.data()+2,narrow(bmp.size()));wire_write<uint32_t>(bmp.data()+10,54);wire_write<uint32_t>(bmp.data()+14,40);wire_write(bmp.data()+18,shape.width);wire_write<int32_t>(bmp.data()+22,int32_t(shape.height));wire_write<uint16_t>(bmp.data()+26,1);wire_write<uint16_t>(bmp.data()+28,32);wire_write(bmp.data()+34,narrow(final.size()));
        const size_t row_bytes=size_t(shape.width)*4;
        for(size_t y=0;y<shape.height;++y){checkpoint();for(size_t x=0;x<row_bytes;x+=4){
            const size_t source=y*row_bytes+x,target=54+(shape.height-1-y)*row_bytes+x;
            bmp[target]=final[source+(shape.format==DXGI_FORMAT_R8G8B8A8_UNORM?2:0)];bmp[target+1]=final[source+1];
            bmp[target+2]=final[source+(shape.format==DXGI_FORMAT_R8G8B8A8_UNORM?0:2)];bmp[target+3]=final[source+3];
        }}
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
