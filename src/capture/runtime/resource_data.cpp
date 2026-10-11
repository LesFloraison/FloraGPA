#include "resource_data.hpp"
#include "objects.hpp"
#include <wrl/client.h>
#include <limits>
#include <cstring>
#include <sstream>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
size_t multiply(size_t a,size_t b){if(b && a>SIZE_MAX/b)throw std::runtime_error("Resource size overflow");return a*b;}
size_t add(size_t a,size_t b){if(a>SIZE_MAX-b)throw std::runtime_error("Resource size overflow");return a+b;}
void check(HRESULT result,const char* api){if(FAILED(result)){std::ostringstream text;text<<api<<" failed: 0x"<<std::hex<<uint32_t(result);throw std::runtime_error(text.str());}}
void surface(SubresourceData& out,DXGI_FORMAT format){
    const UINT f=UINT(format);size_t bits=0,block=0,packed=0;
    if(f>=1 && f<=4)bits=128;
    else if(f>=5 && f<=8)bits=96;
    else if(f>=9 && f<=22)bits=64;
    else if((f>=23 && f<=47) || f==67 || (f>=87 && f<=93) || f==100 || f==101)bits=32;
    else if((f>=48 && f<=59) || f==85 || f==86 || f==114 || f==115)bits=16;
    else if((f>=60 && f<=65) || (f>=111 && f<=113))bits=8;
    else if(f==66)bits=1;
    else if(f==68 || f==69 || f==107)packed=4;
    else if(f==108 || f==109)packed=8;
    else if((f>=70 && f<=72) || (f>=79 && f<=81))block=8;
    else if((f>=73 && f<=78) || (f>=82 && f<=84) || (f>=94 && f<=99))block=16;
    else if(f==102)bits=64;
    else if(f==103 || f==104 || f==105){
        if((out.width&1) || (out.height&1) || out.depth!=1)throw std::runtime_error("Planar texture requires even 2D dimensions");
        out.row_bytes=multiply(out.width,f==103?1:2);out.rows=add(out.height,out.height/2);
        out.slice_bytes=multiply(out.row_bytes,out.rows);return;
    } else throw std::runtime_error("Unsupported DXGI resource layout: "+std::to_string(f));
    if(block){out.row_bytes=multiply((size_t(out.width)+3)/4,block);out.rows=(size_t(out.height)+3)/4;}
    else {out.row_bytes=packed?multiply((size_t(out.width)+1)/2,packed):(add(multiply(out.width,bits),7)/8);out.rows=out.height;}
    out.slice_bytes=multiply(out.row_bytes,out.rows);
}
void resource_flags(UINT flags){
    constexpr UINT supported=D3D11_RESOURCE_MISC_GENERATE_MIPS|D3D11_RESOURCE_MISC_TEXTURECUBE|
        D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS|D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS|
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED|D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
    if(flags & ~supported)throw std::runtime_error("Resource sharing, tiling or special flags require additional capture support");
}
struct LastErrorScope{DWORD value=GetLastError();~LastErrorScope(){SetLastError(value);}};
std::shared_ptr<ObjectRecord> existing_record(IUnknown* object){
    if(auto record=object_record(object))return record;
    ComPtr<IUnknown> identity;check(object->QueryInterface(IID_PPV_ARGS(&identity)),"Snapshot identity");
    return find_native_record(identity.Get());
}
struct PredicationScope{
    ID3D11DeviceContext* context;ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;
    explicit PredicationScope(ID3D11DeviceContext* c):context(c){context->GetPredication(&predicate,&value);context->SetPredication(nullptr,FALSE);}
    ~PredicationScope(){context->SetPredication(predicate.Get(),value);}
};
struct MappedScope{
    ID3D11DeviceContext* context;ID3D11Resource* resource;UINT index;
    ~MappedScope(){context->Unmap(resource,index);}
};
class Readback {
    ID3D11DeviceContext* context_;
    ID3D11Device* device_;
    const SnapshotOptions& options_;
    ULONGLONG start_=GetTickCount64();
public:
    Readback(ID3D11DeviceContext* context,ID3D11Device* device,const SnapshotOptions& options):context_(context),device_(device),options_(options){}
    void checkpoint(){
        if(options_.cancelled && options_.cancelled())throw std::runtime_error("Resource snapshot cancelled");
        if(GetTickCount64()-start_>=options_.timeout_ms)throw std::runtime_error("Resource snapshot timed out");
        check(device_->GetDeviceRemovedReason(),"Snapshot device");
    }
    void read(ID3D11Resource* staging,SubresourceData& sub){
        D3D11_MAPPED_SUBRESOURCE mapped{};
        for(;;){checkpoint();const auto hr=context_->Map(staging,sub.index,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(hr==DXGI_ERROR_WAS_STILL_DRAWING){Sleep(1);continue;}check(hr,"Map snapshot");break;}
        MappedScope unmap{context_,staging,sub.index};copy_pitched(sub,mapped.pData,mapped.RowPitch,mapped.DepthPitch);checkpoint();
    }
};
ComPtr<ID3D11Device> snapshot_device(ID3D11DeviceContext* context,ID3D11Resource* resource){
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw std::runtime_error("Snapshot requires the immediate context");
    ComPtr<ID3D11Device> device,resource_device;context->GetDevice(&device);resource->GetDevice(&resource_device);
    ComPtr<IUnknown> a,b;check(device.As(&a),"Context device identity");check(resource_device.As(&b),"Resource device identity");
    if(a!=b)throw std::runtime_error("Snapshot context/resource device mismatch");return device;
}
}
std::vector<SubresourceData> resource_layout(const ResourceShape& s,size_t byte_limit){
    if(!s.width || !s.height || !s.depth || !s.mip_levels || s.mip_levels>32 || !s.array_size)throw std::runtime_error("Invalid resource dimensions");
    if(s.samples!=1)throw std::runtime_error("MSAA sample-preserving capture is not implemented");
    const size_t count=multiply(s.mip_levels,s.array_size);
    if(count>32768)throw std::runtime_error("Too many resource subresources");
    if(s.dimension==D3D11_RESOURCE_DIMENSION_BUFFER && (count!=1 || s.height!=1 || s.depth!=1))throw std::runtime_error("Invalid buffer layout");
    if(s.dimension!=D3D11_RESOURCE_DIMENSION_BUFFER && s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE1D &&
       s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE2D && s.dimension!=D3D11_RESOURCE_DIMENSION_TEXTURE3D)throw std::runtime_error("Unsupported resource dimension");
    if((s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D && (s.height!=1 || s.depth!=1)) ||
       (s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE2D && s.depth!=1) ||
       (s.dimension==D3D11_RESOURCE_DIMENSION_TEXTURE3D && s.array_size!=1))throw std::runtime_error("Invalid texture dimensions");
    std::vector<SubresourceData> result;result.reserve(count);size_t total=0;
    for(UINT a=0;a<s.array_size;++a)for(UINT m=0;m<s.mip_levels;++m){
        SubresourceData out;out.index=UINT(result.size());out.mip=m;out.array_slice=a;
        out.width=(std::max)(1u,s.width>>m);out.height=(std::max)(1u,s.height>>m);out.depth=(std::max)(1u,s.depth>>m);
        if(s.dimension==D3D11_RESOURCE_DIMENSION_BUFFER){out.row_bytes=out.slice_bytes=s.width;}
        else surface(out,s.format);
        total=add(total,multiply(out.slice_bytes,out.depth));if(total>byte_limit)throw std::runtime_error("Resource capture byte limit exceeded");
        result.push_back(std::move(out));
    }
    return result;
}
void copy_pitched(SubresourceData& target,const void* source,size_t row_pitch,size_t slice_pitch){
    if(!source)throw std::runtime_error("Missing subresource data pointer");
    if(!target.row_bytes || !target.rows || !target.depth || target.slice_bytes!=multiply(target.row_bytes,target.rows))
        throw std::runtime_error("Invalid subresource layout");
    if(target.rows>1 && row_pitch<target.row_bytes)throw std::runtime_error("Subresource row pitch is too small");
    const auto span=add(multiply(target.rows-1,row_pitch),target.row_bytes);
    if(target.depth>1 && slice_pitch<span)throw std::runtime_error("Subresource slice pitch is too small");
    const auto end=add(multiply(target.depth-1,slice_pitch),span);
    if(end>size_t(PTRDIFF_MAX))throw std::runtime_error("Subresource pointer offset overflow");
    std::vector<uint8_t> copy(multiply(target.slice_bytes,target.depth));
    const auto* bytes=static_cast<const uint8_t*>(source);
    for(size_t z=0;z<target.depth;++z)for(size_t y=0;y<target.rows;++y)
        memcpy(copy.data()+z*target.slice_bytes+y*target.row_bytes,bytes+z*slice_pitch+y*row_pitch,target.row_bytes);
    target.bytes=std::move(copy);
}
ResourceShape resource_shape(ID3D11Resource* resource){
    if(!resource)throw std::runtime_error("Missing native resource");
    ResourceShape s;resource->GetType(&s.dimension);
    switch(s.dimension){
    case D3D11_RESOURCE_DIMENSION_BUFFER:{ComPtr<ID3D11Buffer> v;check(resource->QueryInterface(IID_PPV_ARGS(&v)),"Query buffer");D3D11_BUFFER_DESC d{};v->GetDesc(&d);s.width=d.ByteWidth;s.misc_flags=d.MiscFlags;break;}
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:{ComPtr<ID3D11Texture1D> v;check(resource->QueryInterface(IID_PPV_ARGS(&v)),"Query texture1D");D3D11_TEXTURE1D_DESC d{};v->GetDesc(&d);s.width=d.Width;s.mip_levels=d.MipLevels;s.array_size=d.ArraySize;s.format=d.Format;s.misc_flags=d.MiscFlags;break;}
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:{ComPtr<ID3D11Texture2D> v;check(resource->QueryInterface(IID_PPV_ARGS(&v)),"Query texture2D");D3D11_TEXTURE2D_DESC d{};v->GetDesc(&d);s.width=d.Width;s.height=d.Height;s.mip_levels=d.MipLevels;s.array_size=d.ArraySize;s.format=d.Format;s.samples=d.SampleDesc.Count;s.misc_flags=d.MiscFlags;break;}
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:{ComPtr<ID3D11Texture3D> v;check(resource->QueryInterface(IID_PPV_ARGS(&v)),"Query texture3D");D3D11_TEXTURE3D_DESC d{};v->GetDesc(&d);s.width=d.Width;s.height=d.Height;s.depth=d.Depth;s.mip_levels=d.MipLevels;s.format=d.Format;s.misc_flags=d.MiscFlags;break;}
    default:throw std::runtime_error("Unknown resource type");
    }return s;
}
void remember_texture(ID3D11Resource* output,const D3D11_SUBRESOURCE_DATA* initial,const char* api) noexcept{
    if(!output)return;
    try{
        auto record=object_record(output);if(!record)throw std::runtime_error("Texture metadata is unavailable");
        std::vector<SubresourceData> data;
        if(initial){data=resource_layout(resource_shape(unwrap(output)));for(auto& sub:data)copy_pitched(sub,initial[sub.index].pSysMem,initial[sub.index].SysMemPitch,initial[sub.index].SysMemSlicePitch);}
        std::lock_guard lock(record->mutex);record->creation_subresources=std::move(data);record->creation_api=api;
        record->creation_observed=true;record->initial_data_supplied=initial!=nullptr;
    }catch(const std::exception& e){object_gap(api,__uuidof(ID3D11Resource),e.what());}
    catch(...){object_gap(api,__uuidof(ID3D11Resource),"Texture creation data unavailable");}
}
void track_mapping(const void* context_proxy,ID3D11Resource* resource,UINT subresource,bool mapped) noexcept{
    try{
        auto record=object_record(resource),context=object_record(context_proxy);
        if(!record || !context)throw std::runtime_error("Mapped object metadata unavailable");
        std::lock_guard lock(record->mutex);const auto key=std::pair{context->id,subresource};
        if(mapped){if(!record->mapped_subresources.insert(key).second)throw std::runtime_error("Repeated Map without Unmap");}
        else if(!record->mapped_subresources.erase(key))throw std::runtime_error("Unmap without tracked Map");
    }catch(const std::exception& e){object_gap(mapped?"Map":"Unmap",__uuidof(ID3D11Resource),e.what());}
    catch(...){object_gap("Map/Unmap",__uuidof(ID3D11Resource),"Map tracking failed");}
}
ResourceSnapshot snapshot_resource(ID3D11DeviceContext* context,ID3D11Resource* resource,const SnapshotOptions& options){
    LastErrorScope last_error;NativeCall internal;
    if(!context || !resource)throw std::runtime_error("Snapshot requires a context and resource");
    auto record=existing_record(resource);if(!record)throw std::runtime_error("Snapshot resource is not tracked");
    {std::lock_guard lock(record->mutex);if(!record->mapped_subresources.empty())throw std::runtime_error("Cannot snapshot an application-mapped resource");}
    auto* native_context=unwrap(context);auto* native_resource=unwrap(resource);
    auto device=snapshot_device(native_context,native_resource);
    ResourceSnapshot result;result.record=record;result.shape=resource_shape(native_resource);resource_flags(result.shape.misc_flags);
    result.subresources=resource_layout(result.shape,options.byte_limit);
    Readback readback(native_context,device.Get(),options);readback.checkpoint();ComPtr<ID3D11Resource> staging;
    switch(result.shape.dimension){
    case D3D11_RESOURCE_DIMENSION_BUFFER:{D3D11_BUFFER_DESC d{};d.ByteWidth=result.shape.width;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> v;check(device->CreateBuffer(&d,nullptr,&v),"Create snapshot buffer");check(v.As(&staging),"Snapshot resource");break;}
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:{D3D11_TEXTURE1D_DESC d{};d.Width=result.shape.width;d.MipLevels=result.shape.mip_levels;d.ArraySize=result.shape.array_size;d.Format=result.shape.format;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture1D> v;check(device->CreateTexture1D(&d,nullptr,&v),"Create snapshot texture1D");check(v.As(&staging),"Snapshot resource");break;}
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:{D3D11_TEXTURE2D_DESC d{};d.Width=result.shape.width;d.Height=result.shape.height;d.MipLevels=result.shape.mip_levels;d.ArraySize=result.shape.array_size;d.Format=result.shape.format;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> v;check(device->CreateTexture2D(&d,nullptr,&v),"Create snapshot texture2D");check(v.As(&staging),"Snapshot resource");break;}
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:{D3D11_TEXTURE3D_DESC d{};d.Width=result.shape.width;d.Height=result.shape.height;d.Depth=result.shape.depth;d.MipLevels=result.shape.mip_levels;d.Format=result.shape.format;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture3D> v;check(device->CreateTexture3D(&d,nullptr,&v),"Create snapshot texture3D");check(v.As(&staging),"Snapshot resource");break;}
    default:throw std::runtime_error("Unsupported snapshot dimension");
    }
    // CopyResource is predicatable. Preserve the exact application predicate,
    // including its BOOL, on success, cancellation and every exception path.
    PredicationScope predicate(native_context);native_context->CopyResource(staging.Get(),native_resource);native_context->Flush();
    for(auto& sub:result.subresources)readback.read(staging.Get(),sub);
    return result;
}
UavCounterSnapshot snapshot_uav_counter(ID3D11DeviceContext* context,ID3D11UnorderedAccessView* view,const SnapshotOptions& options){
    LastErrorScope last_error;NativeCall internal;
    if(!context || !view)throw std::runtime_error("Counter snapshot requires a context and view");
    UavCounterSnapshot result;result.view=existing_record(view);if(!result.view)throw std::runtime_error("Counter view is not tracked");
    auto* native_context=unwrap(context);auto* native_view=unwrap(view);
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};native_view->GetDesc(&desc);
    if(desc.ViewDimension!=D3D11_UAV_DIMENSION_BUFFER || !(desc.Buffer.Flags&(D3D11_BUFFER_UAV_FLAG_APPEND|D3D11_BUFFER_UAV_FLAG_COUNTER)))
        throw std::runtime_error("View has no independent UAV counter");
    ComPtr<ID3D11Resource> resource;native_view->GetResource(&resource);
    ComPtr<IUnknown> identity;check(resource.As(&identity),"Counter resource identity");auto record=track_native_object(identity.Get());
    {std::lock_guard lock(record->mutex);if(!record->mapped_subresources.empty())throw std::runtime_error("Cannot snapshot an application-mapped resource");}
    resource_flags(resource_shape(resource.Get()).misc_flags);
    auto device=snapshot_device(native_context,resource.Get());Readback readback(native_context,device.Get(),options);readback.checkpoint();
    ResourceShape shape;shape.dimension=D3D11_RESOURCE_DIMENSION_BUFFER;shape.width=4;auto data=resource_layout(shape,options.byte_limit);
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=4;ComPtr<ID3D11Buffer> destination,staging;
    check(device->CreateBuffer(&buffer,nullptr,&destination),"Create counter destination");
    buffer.Usage=D3D11_USAGE_STAGING;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateBuffer(&buffer,nullptr,&staging),"Create counter staging");
    PredicationScope predicate(native_context);native_context->CopyStructureCount(destination.Get(),0,native_view);
    native_context->CopyResource(staging.Get(),destination.Get());native_context->Flush();readback.read(staging.Get(),data[0]);
    memcpy(&result.value,data[0].bytes.data(),sizeof(result.value));return result;
}
}
