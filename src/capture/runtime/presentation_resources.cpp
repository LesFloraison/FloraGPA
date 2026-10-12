#include "presentation_resources.hpp"
#include "metadata.hpp"
#include <dxgi1_6.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <atomic>
namespace flora::capture {
using Microsoft::WRL::ComPtr;
namespace {
const GUID view_owner_key{0xbd65552c,0xf61b,0x4546,{0x8d,0x7f,0x59,0x0e,0x71,0x92,0x94,0x7b}};
struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}};
struct Pair {GUID iid;ComPtr<IUnknown> original,clean;};
struct Entry {std::vector<Pair> interfaces;IUnknown* identity=nullptr;std::atomic<uint64_t> operands=0,views=0;std::mutex report_mutex;Json results=Json::array();};
struct Registry {std::mutex mutex;std::map<void*,std::shared_ptr<Entry>> originals,clean;std::atomic<bool> active=false;};
Registry& registry(){static auto* r=new Registry;return *r;}
std::shared_ptr<Entry> lookup(void* pointer,bool clean){auto& r=registry();if(!r.active.load())return {};std::lock_guard lock(r.mutex);auto& map=clean?r.clean:r.originals;auto found=map.find(pointer);return found==map.end()?nullptr:found->second;}
void check(HRESULT hr,const char* text){if(FAILED(hr))throw std::runtime_error(text);}
}
struct PresentationResource::Impl {std::shared_ptr<Entry> entry;};
PresentationResource::PresentationResource(ID3D11Texture2D* original,ID3D11Texture2D* clean):impl_(std::make_unique<Impl>()){
    LastError preserve;if(!original || !clean || original==clean)throw std::invalid_argument("Distinct presentation resources are required");
    D3D11_TEXTURE2D_DESC a{},b{};original->GetDesc(&a);clean->GetDesc(&b);
    if((a.Format==DXGI_FORMAT_R8G8B8A8_UNORM && b.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS) || (a.Format==DXGI_FORMAT_B8G8R8A8_UNORM && b.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS))b.Format=a.Format;
    if(std::memcmp(&a,&b,sizeof a))throw std::invalid_argument("Presentation resource descriptors differ");
    ComPtr<ID3D11Device> da,db;original->GetDevice(&da);clean->GetDevice(&db);if(da!=db)throw std::invalid_argument("Presentation resources use different devices");
    auto entry=std::make_shared<Entry>();
    for(const auto& iid:{__uuidof(IUnknown),__uuidof(ID3D11DeviceChild),__uuidof(ID3D11Resource),__uuidof(ID3D11Texture2D),__uuidof(ID3D11Texture2D1),__uuidof(IDXGIObject),__uuidof(IDXGIDeviceSubObject),__uuidof(IDXGIResource),__uuidof(IDXGIResource1),__uuidof(IDXGISurface),__uuidof(IDXGISurface1),__uuidof(IDXGISurface2)}){
        Pair pair;pair.iid=iid;const auto have=original->QueryInterface(iid,reinterpret_cast<void**>(pair.original.GetAddressOf()));
        if(FAILED(have))continue;
        check(clean->QueryInterface(iid,reinterpret_cast<void**>(pair.clean.GetAddressOf())),"Clean buffer lacks a native resource interface");
        if(iid==__uuidof(IUnknown))entry->identity=pair.original.Get();entry->interfaces.push_back(std::move(pair));
    }
    auto& r=registry();std::lock_guard lock(r.mutex);
    for(const auto& pair:entry->interfaces)if(r.originals.contains(pair.original.Get()) || r.clean.contains(pair.clean.Get()))throw std::logic_error("Presentation resource already registered");
    try{for(const auto& pair:entry->interfaces){r.originals.emplace(pair.original.Get(),entry);r.clean.emplace(pair.clean.Get(),entry);}}
    catch(...){for(auto it=r.originals.begin();it!=r.originals.end();)if(it->second==entry)it=r.originals.erase(it);else ++it;for(auto it=r.clean.begin();it!=r.clean.end();)if(it->second==entry)it=r.clean.erase(it);else ++it;throw;}
    impl_->entry=std::move(entry);
    r.active=true;
}
PresentationResource::~PresentationResource(){
    LastError preserve;auto& r=registry();const auto entry=impl_->entry;
    {std::lock_guard lock(r.mutex);for(auto it=r.originals.begin();it!=r.originals.end();)if(it->second==entry)it=r.originals.erase(it);else ++it;for(auto it=r.clean.begin();it!=r.clean.end();)if(it->second==entry)it=r.clean.erase(it);else ++it;r.active=!r.clean.empty();}
    // Release native COM interfaces outside the registry lock.
    impl_.reset();
}
void* presentation_operand(void* native,REFIID iid){
    LastError preserve;const auto entry=lookup(native,false);if(!entry)return native;
    ++entry->operands;
    for(const auto& pair:entry->interfaces)if(pair.iid==iid)return pair.clean.Get();
    throw std::runtime_error("Unsupported presentation resource operand interface");
}
void presentation_returned(REFIID iid,void** returned){
    LastError preserve;if(!returned || !*returned)return;const auto entry=lookup(*returned,true);if(!entry)return;
    void* original=nullptr;check(entry->identity->QueryInterface(iid,&original),"Cannot restore native presentation identity");
    static_cast<IUnknown*>(*returned)->Release();*returned=original;
}
ComPtr<IUnknown> presentation_identity(IUnknown* identity){
    LastError preserve;const auto entry=lookup(identity,true);return ComPtr<IUnknown>(entry?entry->identity:identity);
}
void retain_presentation_view(IUnknown* native){
    if(!registry().active.load())return;
    LastError preserve;ComPtr<ID3D11View> view;if(FAILED(native->QueryInterface(IID_PPV_ARGS(&view))))return;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);const auto entry=lookup(resource.Get(),true);if(!entry)return;
    // Native views of clean storage must retain the corresponding real buffer,
    // just as a native swap-buffer view does, even after its proxy is released.
    UINT size=sizeof(IUnknown*);ComPtr<IUnknown> prior;const auto hr=view->GetPrivateData(view_owner_key,&size,prior.GetAddressOf());
    if(SUCCEEDED(hr)){if(prior.Get()!=entry->identity)throw std::runtime_error("Presentation view lifetime GUID is occupied");return;}
    if(hr!=DXGI_ERROR_NOT_FOUND)throw std::runtime_error("Cannot query presentation view lifetime");
    check(view->SetPrivateDataInterface(view_owner_key,entry->identity),"Cannot retain presentation buffer for view");
    ++entry->views;
}
ComPtr<ID3D11Resource> presentation_validation_resource(ID3D11Resource* clean){
    LastError preserve;const auto entry=lookup(clean,true);ComPtr<ID3D11Resource> result;
    if(entry)check(entry->identity->QueryInterface(IID_PPV_ARGS(&result)),"Presentation validation resource missing");return result;
}
bool presentation_private_key(REFGUID key) noexcept{return key==view_owner_key;}
Json presentation_status(){
    LastError preserve;std::vector<std::shared_ptr<Entry>> entries;
    {auto& r=registry();std::lock_guard lock(r.mutex);for(const auto& [pointer,entry]:r.originals)if(pointer==entry->identity)entries.push_back(entry);}
    Json result=Json::array();for(const auto& entry:entries){const auto record=find_native_record(entry->identity);std::lock_guard lock(entry->report_mutex);result.push_back({{"resource",record?Json(record->id):Json()},{"gpu_operands",entry->operands.load()},{"views",entry->views.load()},{"view_results",entry->results}});}return result;
}
void presentation_view_result(ID3D11Resource* original,const char* api,HRESULT validation,HRESULT result,UINT format){LastError preserve;const auto entry=lookup(original,false);if(!entry)return;std::lock_guard lock(entry->report_mutex);if(entry->results.size()<16)entry->results.push_back({{"api",api},{"validation",uint32_t(validation)},{"result",uint32_t(result)},{"format",format}});}
}
