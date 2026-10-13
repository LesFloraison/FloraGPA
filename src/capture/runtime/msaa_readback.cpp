#include "msaa_readback.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <stdexcept>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr,const char* api) {
    if(FAILED(hr))throw std::runtime_error(std::string(api)+" failed: "+std::to_string(uint32_t(hr)));
}
struct LastError { DWORD value=GetLastError(); ~LastError(){SetLastError(value);} };
struct Unmap { ID3D11DeviceContext* context;ID3D11Buffer* buffer;~Unmap(){context->Unmap(buffer,0);} };
std::shared_ptr<ObjectRecord> tracked(IUnknown* object) {
    if(auto record=object_record(object))return record;
    ComPtr<IUnknown> identity;check(object->QueryInterface(IID_PPV_ARGS(&identity)),"MSAA identity");
    auto record=find_native_record(identity.Get());
    if(!record)throw std::runtime_error("MSAA snapshot object is not tracked");
    return record;
}
}
MsaaSamples snapshot_msaa_samples(ID3D11DeviceContext* context,ID3D11Texture2D* texture,const SnapshotOptions& options) {
    LastError last;NativeCall internal;
    if(!context || !texture)throw std::runtime_error("MSAA snapshot requires context and texture");
    if(object_status().at("interface_gaps").get<uint64_t>())
        throw std::runtime_error("MSAA snapshot has prior interface capability gaps");
    const auto start=GetTickCount64();
    auto* native=unwrap(context);auto* source=unwrap(texture);
    auto cr=tracked(context),tr=tracked(texture);
    if(native->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)
        throw std::runtime_error("MSAA snapshot requires immediate context");
    {std::lock_guard guard(cr->mutex);if(!cr->active_queries.empty())throw std::runtime_error("MSAA snapshot inside active query unsupported");}
    {std::lock_guard guard(tr->mutex);if(!tr->mapped_subresources.empty() || tr->external_mapping)throw std::runtime_error("MSAA snapshot resource is mapped");}
    ComPtr<ID3D11Predicate> predicate;BOOL predicate_value=FALSE;native->GetPredication(&predicate,&predicate_value);
    if(predicate)throw std::runtime_error("MSAA snapshot with predication unsupported");
    ComPtr<ID3D11Device> device,source_device;native->GetDevice(&device);source->GetDevice(&source_device);
    ComPtr<IUnknown> a,b;check(device.As(&a),"MSAA context device");check(source_device.As(&b),"MSAA texture device");
    if(a!=b)throw std::runtime_error("MSAA snapshot device mismatch");
    if(device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0)throw std::runtime_error("MSAA readback requires feature level 11");
    auto checkpoint=[&] {
        if(options.cancelled && options.cancelled())throw std::runtime_error("MSAA snapshot cancelled");
        if(GetTickCount64()-start>=options.timeout_ms)throw std::runtime_error("MSAA snapshot timed out");
        check(device->GetDeviceRemovedReason(),"MSAA snapshot device");
    };
    checkpoint();D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    const bool depth_stencil=(desc.BindFlags&D3D11_BIND_DEPTH_STENCIL)!=0;
    auto result=msaa_sample_layout(resource_shape(source),desc.SampleDesc.Quality,options.byte_limit,depth_stencil);result.resource=tr->id;
    const auto access=msaa_sample_access(desc.Format,depth_stencil);
    for(const auto format:{access.view,access.stencil_view})if(format!=DXGI_FORMAT_UNKNOWN){
        UINT support=0;check(device->CheckFormatSupport(format,&support),"MSAA sample view support");
        if(!(support&D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD))throw std::runtime_error("Device lacks MSAA sample loads");
    }
    // Copy through a compatible typeless resource. Color and D16/D32 use
    // integer loads. Combined depth/stencil formats have separate aspect views.
    desc.Format=access.storage;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags=desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> readable;check(device->CreateTexture2D(&desc,nullptr,&readable),"Create MSAA private copy");
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=access.view;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;sd.Texture2DMSArray.ArraySize=desc.ArraySize;
    ComPtr<ID3D11ShaderResourceView> srv;check(device->CreateShaderResourceView(readable.Get(),&sd,&srv),"Create MSAA sample view");
    ComPtr<ID3D11ShaderResourceView> stencil_srv;
    if(access.stencil_view!=DXGI_FORMAT_UNKNOWN){sd.Format=access.stencil_view;check(device->CreateShaderResourceView(readable.Get(),&sd,&stencil_srv),"Create MSAA stencil view");}
    const UINT pixels=desc.Width*desc.Height;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=pixels*16;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=16;
    ComPtr<ID3D11Buffer> output,staging;check(device->CreateBuffer(&bd,nullptr,&output),"Create MSAA sample buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=pixels;
    ComPtr<ID3D11UnorderedAccessView> uav;check(device->CreateUnorderedAccessView(output.Get(),&ud,&uav),"Create MSAA sample UAV");
    bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(device->CreateBuffer(&bd,nullptr,&staging),"Create MSAA sample staging");
    std::string shader=R"(
Texture2DMSArray<uint4> source:register(t0);
RWStructuredBuffer<uint4> destination:register(u0);
cbuffer Plane:register(b0){uint width,height,layer,sample;}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
    if(p.x<width && p.y<height)destination[p.y*width+p.x]=source.Load(int3(p.xy,layer),sample);
})";
    if(stencil_srv){
        shader=R"(
Texture2DMSArray<float> depth:register(t0);
Texture2DMSArray<uint2> stencil:register(t1);
RWStructuredBuffer<uint4> destination:register(u0);
cbuffer Plane:register(b0){uint width,height,layer,sample;}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){
    if(p.x>=width || p.y>=height)return;
    float d=depth.Load(int3(p.xy,layer),sample);
    uint s=stencil.Load(int3(p.xy,layer),sample).y;
)";
        if(access.encoding==MsaaSampleEncoding::depth24_stencil8)
            shader+="precise float scaled=d*16777215.0;destination[p.y*width+p.x]=uint4(uint(round(scaled))|(s<<24),0,0,0);}";
        else shader+="destination[p.y*width+p.x]=uint4(asuint(d),s,0,0);}";
    }
    ComPtr<ID3DBlob> code,errors;
    const auto compiled=D3DCompile(shader.data(),shader.size(),"FloraCapture sample readback",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if(FAILED(compiled) && errors)throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()));
    check(compiled,"Compile MSAA readback");
    ComPtr<ID3D11ComputeShader> cs;check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&cs),"Create MSAA readback shader");
    ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred),"Create MSAA readback context");
    for(auto& plane:result.planes) {
        checkpoint();const UINT values[]{desc.Width,desc.Height,plane.array_slice,plane.sample};
        D3D11_BUFFER_DESC constants_desc{};constants_desc.ByteWidth=16;constants_desc.Usage=D3D11_USAGE_IMMUTABLE;constants_desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{values,0,0};ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&constants_desc,&initial,&constants),"Create MSAA plane constants");
        // Copy once, before any sample loads. All source bytes stay untouched.
        if(plane.array_slice==0 && plane.sample==0)deferred->CopyResource(readable.Get(),source);
        deferred->CSSetShader(cs.Get(),nullptr,0);ID3D11ShaderResourceView* views[]{srv.Get(),stencil_srv.Get()};deferred->CSSetShaderResources(0,2,views);
        auto* c=constants.Get();deferred->CSSetConstantBuffers(0,1,&c);auto* u=uav.Get();deferred->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        deferred->Dispatch((desc.Width+7)/8,(desc.Height+7)/8,1);
        deferred->CopyResource(staging.Get(),output.Get());
        ComPtr<ID3D11CommandList> list;check(deferred->FinishCommandList(FALSE,&list),"Finish MSAA sample readback");
        checkpoint();native->ExecuteCommandList(list.Get(),TRUE);native->Flush();
        D3D11_MAPPED_SUBRESOURCE map{};
        for(;;){checkpoint();const auto hr=native->Map(staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING){Sleep(1);continue;}check(hr,"Map MSAA sample readback");break;}
        Unmap unmap{native,staging.Get()};
        if(!map.pData)throw std::runtime_error("MSAA sample readback returned null data");
        plane.bytes.resize(plane.row_bytes*desc.Height);
        const auto* words=static_cast<const uint8_t*>(map.pData);size_t to=0;
        for(UINT y=0;y<desc.Height;++y){checkpoint();for(UINT x=0;x<desc.Width;++x)
            for(UINT component=0;component<access.components;++component){
                memcpy(plane.bytes.data()+to,words+(size_t(y)*desc.Width+x)*16+component*4,access.component_bytes);to+=access.component_bytes;
            }}
    }
    checkpoint();return result;
}
}
