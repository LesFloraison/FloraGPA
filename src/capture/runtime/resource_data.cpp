#include "resource_data.hpp"
#include "objects.hpp"
#include "presentation_resources.hpp"
#include <wrl/client.h>
#include <limits>
#include <cstring>
#include <sstream>

namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result,const char* api){if(FAILED(result)){std::ostringstream text;text<<api<<" failed: 0x"<<std::hex<<uint32_t(result);throw std::runtime_error(text.str());}}
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
ResourceShape resource_shape(ID3D11Resource* resource){
    if(!resource)throw std::runtime_error("Missing native resource");
    // Private typeless storage is byte-compatible with the public swap format.
    // Snapshots and wire descriptors describe the application resource.
    auto original=presentation_validation_resource(resource);if(original)resource=original.Get();
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
void track_mapping(const void* context_proxy,ID3D11Resource* resource,UINT subresource,bool mapped,
    D3D11_MAP type,UINT flags,HRESULT result,const D3D11_MAPPED_SUBRESOURCE* storage) noexcept{
    try{
        auto record=object_record(resource),context=object_record(context_proxy);
        if(!record || !context)throw std::runtime_error("Mapped object metadata unavailable");
        std::lock_guard lock(record->mutex);const auto key=std::pair{context->id,subresource};
        if(mapped){
            MappingStamp stamp;stamp.generation=++record->mapping_serial;stamp.thread=GetCurrentThreadId();stamp.type=type;stamp.flags=flags;stamp.result=result;
            if(storage){stamp.row_pitch=storage->RowPitch;stamp.depth_pitch=storage->DepthPitch;stamp.cpu_storage=storage->pData!=nullptr;}
            if(!record->mapped_subresources.emplace(key,stamp).second)throw std::runtime_error("Repeated Map without Unmap");
        }
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
