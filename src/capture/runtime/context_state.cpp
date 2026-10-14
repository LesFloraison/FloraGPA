#include "context_state.hpp"
#include "objects.hpp"
#include <stdexcept>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
struct LastErrorScope { DWORD value=GetLastError(); ~LastErrorScope(){SetLastError(value);} };
void check(HRESULT hr,const char* api){if(FAILED(hr))throw std::runtime_error(api);}
// Getters AddRef every returned slot, even when several slots alias one object.
// Keep RAII ownership before allocation/metadata lookup can throw.
template<class T,size_t N> struct Returned {
    std::array<T*,N> values{};
    ~Returned(){for(auto* value:values)if(value)value->Release();}
    T** data(){return values.data();}
};
class Builder {
    ID3D11DeviceContext* context_;
    bool deferred_;
public:
    ContextSnapshot result;
    explicit Builder(ID3D11DeviceContext* context,bool deferred):context_(context),deferred_(deferred){}
    ObjectId retain(IUnknown* object){
        if(!object)return 0;
        ComPtr<IUnknown> native;check(object->QueryInterface(IID_PPV_ARGS(&native)),"Snapshot object identity failed");
        auto record=track_native_object(native.Get());
        if(result.objects.contains(record->id))return record->id;
        result.objects.emplace(record->id,RetainedObject{record,native});
        ComPtr<ID3D11View> view;
        if(SUCCEEDED(native.As(&view))){
            ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
            if(retain(resource.Get())!=record->resource)throw std::runtime_error("Snapshot view/resource identity changed");
        }
        ComPtr<ID3D11Resource> resource;
        if(SUCCEEDED(native.As(&resource))){
            const auto shape=resource_shape(resource.Get());
            if(shape.misc_flags&D3D11_RESOURCE_MISC_RESOURCE_CLAMP){
                if(deferred_)result.gaps.push_back("Deferred resource MinLOD requires execution-time state");
                else result.bindings.resource_min_lod.emplace(record->id,context_->GetResourceMinLOD(resource.Get()));
            }
        }
        // Hidden counters are not getters: deferred lowering verifies ordered
        // explicit resets/KEEP before admitting any consuming Draw/Dispatch.
        return record->id;
    }
    template<class T,size_t N> void retain(const Returned<T,N>& source,std::array<ObjectId,N>& target){
        for(size_t i=0;i<N;++i)target[i]=retain(source.values[i]);
    }
};
}
void ContextSnapshot::require_supported_bindings() const {
    if(!gaps.empty())throw std::runtime_error(gaps.front());
}
namespace {
ContextSnapshot snapshot_pipeline(ID3D11DeviceContext* application_context,bool deferred){
    LastErrorScope last_error;NativeCall internal;
    if(!application_context || !object_record(application_context))throw std::runtime_error("Snapshot context is not tracked");
    auto* context=unwrap(application_context);
    if(context->GetType()!=(deferred?D3D11_DEVICE_CONTEXT_DEFERRED:D3D11_DEVICE_CONTEXT_IMMEDIATE))
        throw std::runtime_error(deferred?"Pipeline snapshot requires a deferred context":"Pipeline snapshot requires an immediate context");
    ComPtr<ID3D11Device> device;context->GetDevice(&device);
    check(device->GetDeviceRemovedReason(),"Pipeline snapshot device removed");
    Builder out(context,deferred);auto& state=out.result.bindings;
    state.context=out.retain(context);state.device=out.retain(device.Get());state.feature_level=device->GetFeatureLevel();
    if(state.feature_level<D3D_FEATURE_LEVEL_11_0)throw std::runtime_error("Pipeline snapshot requires feature level 11.0 or newer");
    state.uav_slots=state.feature_level>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
    ComPtr<ID3D11DeviceContext1> context1;context->QueryInterface(IID_PPV_ARGS(&context1));state.constant_ranges=bool(context1);
    ComPtr<ID3D11DeviceContext3> context3;
    if(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context3)))){
        BOOL protected_state=FALSE;context3->GetHardwareProtectionState(&protected_state);
        if(protected_state)out.result.gaps.push_back("Hardware-protected context state is unsupported");
    }
#define STAGE(NAME,TYPE,INDEX) { \
    auto& stage=state.shaders[size_t(ShaderStage::INDEX)]; \
    ComPtr<TYPE> shader;Returned<ID3D11ClassInstance,D3D11_SHADER_MAX_INTERFACES> classes;UINT count=UINT(classes.values.size()); \
    context->NAME##GetShader(&shader,classes.data(),&count);stage.shader=out.retain(shader.Get()); \
    if(count>classes.values.size())throw std::runtime_error("Shader class count exceeds snapshot capacity"); \
    for(UINT i=0;i<count;++i)stage.classes.push_back(out.retain(classes.values[i])); \
    if(count)out.result.gaps.push_back(#NAME " dynamic shader linkage reconstruction is unsupported"); \
    Returned<ID3D11Buffer,D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers; \
    if(context1)context1->NAME##GetConstantBuffers1(0,UINT(buffers.values.size()),buffers.data(),stage.first_constants.data(),stage.constant_counts.data()); \
    else {context->NAME##GetConstantBuffers(0,UINT(buffers.values.size()),buffers.data()); \
        for(size_t i=0;i<buffers.values.size();++i)if(buffers.values[i])stage.constant_counts[i]=D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;} \
    out.retain(buffers,stage.buffers); \
    Returned<ID3D11ShaderResourceView,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources; \
    context->NAME##GetShaderResources(0,UINT(resources.values.size()),resources.data());out.retain(resources,stage.resources); \
    Returned<ID3D11SamplerState,D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers; \
    context->NAME##GetSamplers(0,UINT(samplers.values.size()),samplers.data());out.retain(samplers,stage.samplers); \
}
    STAGE(VS,ID3D11VertexShader,vertex)
    STAGE(HS,ID3D11HullShader,hull)
    STAGE(DS,ID3D11DomainShader,domain)
    STAGE(GS,ID3D11GeometryShader,geometry)
    STAGE(PS,ID3D11PixelShader,pixel)
    STAGE(CS,ID3D11ComputeShader,compute)
#undef STAGE
    ComPtr<ID3D11InputLayout> layout;context->IAGetInputLayout(&layout);state.input_layout=out.retain(layout.Get());
    Returned<ID3D11Buffer,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices;
    context->IAGetVertexBuffers(0,UINT(vertices.values.size()),vertices.data(),state.strides.data(),state.offsets.data());out.retain(vertices,state.vertices);
    ComPtr<ID3D11Buffer> indices;context->IAGetIndexBuffer(&indices,&state.index_format,&state.index_offset);state.index_buffer=out.retain(indices.Get());
    context->IAGetPrimitiveTopology(&state.topology);
    ComPtr<ID3D11RasterizerState> rasterizer;context->RSGetState(&rasterizer);state.rasterizer=out.retain(rasterizer.Get());
    UINT count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    state.viewports.resize(count);context->RSGetViewports(&count,state.viewports.data());
    if(count>state.viewports.size())throw std::runtime_error("Viewport count exceeds snapshot capacity");state.viewports.resize(count);
    count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    state.scissors.resize(count);context->RSGetScissorRects(&count,state.scissors.data());
    if(count>state.scissors.size())throw std::runtime_error("Scissor count exceeds snapshot capacity");state.scissors.resize(count);
    ComPtr<ID3D11BlendState> blend;context->OMGetBlendState(&blend,state.blend_factor.data(),&state.sample_mask);state.blend=out.retain(blend.Get());
    ComPtr<ID3D11DepthStencilState> depth;context->OMGetDepthStencilState(&depth,&state.stencil_ref);state.depth_stencil=out.retain(depth.Get());
    Returned<ID3D11RenderTargetView,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;ComPtr<ID3D11DepthStencilView> dsv;
    context->OMGetRenderTargets(UINT(targets.values.size()),targets.data(),&dsv);out.retain(targets,state.render_targets);state.depth_view=out.retain(dsv.Get());
    Returned<ID3D11UnorderedAccessView,D3D11_1_UAV_SLOT_COUNT> om,cs;
    // Query UAVs separately with NumRTVs=0, so slots below 8 are not omitted.
    context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,state.uav_slots,om.data());out.retain(om,state.om_uavs);
    context->CSGetUnorderedAccessViews(0,state.uav_slots,cs.data());out.retain(cs,state.cs_uavs);
    Returned<ID3D11Buffer,D3D11_SO_BUFFER_SLOT_COUNT> stream;context->SOGetTargets(UINT(stream.values.size()),stream.data());out.retain(stream,state.stream_targets);
    for(auto id:state.stream_targets)if(id){out.result.gaps.push_back("SO append position cannot be recovered from SOGetTargets");break;}
    ComPtr<ID3D11Predicate> predicate;context->GetPredication(&predicate,&state.predicate_value);state.predicate=out.retain(predicate.Get());
    if(predicate)out.result.gaps.push_back("Active predicate query state reconstruction is unsupported");
    check(device->GetDeviceRemovedReason(),"Pipeline snapshot device removed");return std::move(out.result);
}
}
ContextSnapshot snapshot_context(ID3D11DeviceContext* context){return snapshot_pipeline(context,false);}
ContextSnapshot snapshot_deferred_context(ID3D11DeviceContext* context){return snapshot_pipeline(context,true);}
}
