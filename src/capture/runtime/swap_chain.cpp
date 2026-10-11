#include "swap_chain.hpp"
#include "factory.hpp"
#include "objects.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace flora::capture {
namespace {
class SwapChain;
struct Observation {
    std::uint64_t id=0;
    UINT width=0,height=0,format=0;
    std::uintptr_t window=0;
    std::uint64_t present_calls=0,present1_calls=0,present_tests=0,presented=0;
    std::uint64_t unsuccessful_presents=0,resizes=0,failed_resizes=0,escape_count=0;
    HRESULT last_present=S_OK,last_resize=S_OK;
    std::string last_escape;
};
struct Registry {
    std::mutex mutex;
    std::map<IUnknown*,SwapChain*> objects;
    std::map<IDXGISwapChain*,SwapChain*> wrappers;
    std::map<std::uint64_t,Observation> observations;
    std::uint64_t next_id=1,created=0,destroyed=0;
};
std::atomic<unsigned> tracking_failures{0};
// Intentionally process-lifetime: DXGI/CRT teardown order is not under our
// control, and Release may be called while other DLLs are detaching.
Registry& registry(){static auto* value=new Registry;return *value;}

std::string guid_text(REFIID iid){
    char text[40]{};
    sprintf_s(text,"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        iid.Data1,iid.Data2,iid.Data3,iid.Data4[0],iid.Data4[1],iid.Data4[2],iid.Data4[3],
        iid.Data4[4],iid.Data4[5],iid.Data4[6],iid.Data4[7]);
    return text;
}

class SwapChain final:public IDXGISwapChain4,public IDXGISwapChainMedia {
    IDXGISwapChain* real_;
    IUnknown* identity_; // Weak canonical identity; real_ owns its lifetime.
    std::uint64_t id_;
    ULONG references_=1; // Guarded by Registry::mutex.

    template<class F> void observe(F&& f) noexcept {
        const DWORD error=GetLastError();
        try{auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);f(r.observations.at(id_));}
        catch(...){++tracking_failures;}
        SetLastError(error);
    }
    template<class T> void escaped(const char* method,REFIID iid,HRESULT result,T** output) noexcept {
        if(SUCCEEDED(result) && output && *output)
            observe([&](Observation& o){++o.escape_count;o.last_escape=std::string(method)+": "+guid_text(iid);});
    }
    template<class Interface,class F> auto extended(F&& f)->decltype(f(static_cast<Interface*>(nullptr))) {
        // Keep a reference to the exact interface for the duration of the call,
        // including COM tear-offs. Do not cache a released interface pointer.
        const DWORD before=GetLastError();Interface* view=nullptr;
        const HRESULT query=real_->QueryInterface(__uuidof(Interface),reinterpret_cast<void**>(&view));
        if(FAILED(query)){
            ++tracking_failures;SetLastError(before);
            using Result=decltype(f(view));
            if constexpr(std::is_same_v<Result,HRESULT>)return query;
            else return Result{};
        }
        SetLastError(before);auto result=f(view);const DWORD after=GetLastError();
        view->Release();SetLastError(after);return result;
    }
    void presented(UINT flags,HRESULT result,bool extended_call) noexcept {
        observe([&](Observation& o){
            ++o.present_calls;if(extended_call)++o.present1_calls;o.last_present=result;
            if(flags&DXGI_PRESENT_TEST)++o.present_tests;
            else if(result==S_OK)++o.presented;
            else ++o.unsuccessful_presents;
        });
    }
    void resized(HRESULT result) noexcept {
        const DWORD error=GetLastError();DXGI_SWAP_CHAIN_DESC desc{};
        const bool have_desc=SUCCEEDED(result) && SUCCEEDED(real_->GetDesc(&desc));
        observe([&](Observation& o){
            ++o.resizes;o.last_resize=result;if(FAILED(result))++o.failed_resizes;
            if(have_desc){o.width=desc.BufferDesc.Width;o.height=desc.BufferDesc.Height;o.format=desc.BufferDesc.Format;o.window=reinterpret_cast<std::uintptr_t>(desc.OutputWindow);}
        });SetLastError(error);
    }
public:
    SwapChain(IDXGISwapChain* real,IUnknown* identity,std::uint64_t id):real_(real),identity_(identity),id_(id){}
    // Caller holds registry lock; consumes another native returned reference.
    void adopt(IDXGISwapChain* returned){++references_;real_->AddRef();returned->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        const HRESULT result=real_->QueryInterface(iid,output);const DWORD error=GetLastError();
        if(SUCCEEDED(result) && output && *output){
            if(iid==__uuidof(IUnknown) || iid==__uuidof(IDXGIObject) || iid==__uuidof(IDXGIDeviceSubObject) ||
               iid==__uuidof(IDXGISwapChain) || iid==__uuidof(IDXGISwapChain1) || iid==__uuidof(IDXGISwapChain2) ||
               iid==__uuidof(IDXGISwapChain3) || iid==__uuidof(IDXGISwapChain4) || iid==__uuidof(IDXGISwapChainMedia)){
                // Transfer the successful QI reference to our owning base
                // interface while returning a stable application identity.
                AddRef();static_cast<IUnknown*>(*output)->Release();
                if(iid==__uuidof(IDXGISwapChainMedia))*output=static_cast<IDXGISwapChainMedia*>(this);
                else *output=static_cast<IDXGISwapChain4*>(this);
            }else { escaped("QueryInterface",iid,result,output); object_gap("IDXGISwapChain::QueryInterface",iid,"Interface forwarding is not implemented"); }
        }
        SetLastError(error);return result;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        const DWORD before=GetLastError();
        auto& r=registry();{std::lock_guard<std::mutex> lock(r.mutex);++references_;}
        SetLastError(before);return real_->AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const DWORD before=GetLastError();
        auto& r=registry();bool destroy=false;IDXGISwapChain* real=real_;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            if(--references_==0){r.objects.erase(identity_);r.wrappers.erase(this);r.observations.erase(id_);++r.destroyed;destroy=true;}
        }
        SetLastError(before);const ULONG result=real->Release();const DWORD after=GetLastError();
        if(destroy)delete this;SetLastError(after);return result;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void** output) override {
        const auto result=real_->GetParent(iid,output);
        if(SUCCEEDED(result))wrap_factory(iid,output);
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** output) override {
        const auto result=real_->GetDevice(iid,output);if(SUCCEEDED(result))wrap_object(iid,output,"IDXGISwapChain::GetDevice");return result;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index,REFIID iid,void** output) override {
        const auto result=real_->GetBuffer(index,iid,output);if(SUCCEEDED(result))wrap_object(iid,output,"IDXGISwapChain::GetBuffer");return result;
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** output) override {
        const auto result=real_->GetContainingOutput(output);if(SUCCEEDED(result))wrap_object(output,"IDXGISwapChain::GetContainingOutput");return result;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fullscreen,IDXGIOutput** output) override {
        const auto result=real_->GetFullscreenState(fullscreen,output);if(SUCCEEDED(result))wrap_object(output,"IDXGISwapChain::GetFullscreenState");return result;
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** output) override {
        const auto result=extended<IDXGISwapChain1>([&](auto* p){return p->GetRestrictToOutput(output);});
        if(SUCCEEDED(result))wrap_object(output,"IDXGISwapChain::GetRestrictToOutput");return result;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID iid,void** output) override {
        const auto result=extended<IDXGISwapChain1>([&](auto* p){return p->GetCoreWindow(iid,output);});
        escaped("GetCoreWindow (window not wrapped)",iid,result,output);return result;
    }
    HRESULT STDMETHODCALLTYPE Present(UINT interval,UINT flags) override {
        const auto result=real_->Present(interval,flags);presented(flags,result,false);return result;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters) override {
        const auto result=extended<IDXGISwapChain1>([&](auto* p){return p->Present1(interval,flags,parameters);});
        presented(flags,result,true);return result;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags) override {
        const auto result=real_->ResizeBuffers(count,width,height,format,flags);resized(result);return result;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags,const UINT* masks,IUnknown* const* queues) override {
        const auto result=extended<IDXGISwapChain3>([&](auto* p){return p->ResizeBuffers1(count,width,height,format,flags,masks,queues);});
        resized(result);return result;
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatisticsMedia(DXGI_FRAME_STATISTICS_MEDIA* statistics) override {
        return extended<IDXGISwapChainMedia>([&](auto* p){return p->GetFrameStatisticsMedia(statistics);});
    }
    HRESULT STDMETHODCALLTYPE SetPresentDuration(UINT duration) override {
        return extended<IDXGISwapChainMedia>([&](auto* p){return p->SetPresentDuration(duration);});
    }
    HRESULT STDMETHODCALLTYPE CheckPresentDurationSupport(UINT duration,UINT* smaller,UINT* larger) override {
        return extended<IDXGISwapChainMedia>([&](auto* p){return p->CheckPresentDurationSupport(duration,smaller,larger);});
    }
#define BASE(result,name,args,call) result STDMETHODCALLTYPE name args override{return real_->name call;}
#define EXT(version,result,name,args,call) result STDMETHODCALLTYPE name args override{return extended<IDXGISwapChain##version>([&](auto* p){return p->name call;});}
    BASE(HRESULT,SetPrivateData,(REFGUID name,UINT size,const void* data),(name,size,data))
    BASE(HRESULT,SetPrivateDataInterface,(REFGUID name,const IUnknown* object),(name,object))
    BASE(HRESULT,GetPrivateData,(REFGUID name,UINT* size,void* data),(name,size,data))
    BASE(HRESULT,SetFullscreenState,(BOOL fullscreen,IDXGIOutput* output),(fullscreen,unwrap(output)))
    BASE(HRESULT,GetDesc,(DXGI_SWAP_CHAIN_DESC* desc),(desc))
    BASE(HRESULT,ResizeTarget,(const DXGI_MODE_DESC* desc),(desc))
    BASE(HRESULT,GetFrameStatistics,(DXGI_FRAME_STATISTICS* stats),(stats))
    BASE(HRESULT,GetLastPresentCount,(UINT* count),(count))
    EXT(1,HRESULT,GetDesc1,(DXGI_SWAP_CHAIN_DESC1* desc),(desc))
    EXT(1,HRESULT,GetFullscreenDesc,(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* desc),(desc))
    EXT(1,HRESULT,GetHwnd,(HWND* window),(window))
    EXT(1,BOOL,IsTemporaryMonoSupported,(),())
    EXT(1,HRESULT,SetBackgroundColor,(const DXGI_RGBA* color),(color))
    EXT(1,HRESULT,GetBackgroundColor,(DXGI_RGBA* color),(color))
    EXT(1,HRESULT,SetRotation,(DXGI_MODE_ROTATION rotation),(rotation))
    EXT(1,HRESULT,GetRotation,(DXGI_MODE_ROTATION* rotation),(rotation))
    EXT(2,HRESULT,SetSourceSize,(UINT width,UINT height),(width,height))
    EXT(2,HRESULT,GetSourceSize,(UINT* width,UINT* height),(width,height))
    EXT(2,HRESULT,SetMaximumFrameLatency,(UINT latency),(latency))
    EXT(2,HRESULT,GetMaximumFrameLatency,(UINT* latency),(latency))
    EXT(2,HANDLE,GetFrameLatencyWaitableObject,(),())
    EXT(2,HRESULT,SetMatrixTransform,(const DXGI_MATRIX_3X2_F* matrix),(matrix))
    EXT(2,HRESULT,GetMatrixTransform,(DXGI_MATRIX_3X2_F* matrix),(matrix))
    EXT(3,UINT,GetCurrentBackBufferIndex,(),())
    EXT(3,HRESULT,CheckColorSpaceSupport,(DXGI_COLOR_SPACE_TYPE color,UINT* support),(color,support))
    EXT(3,HRESULT,SetColorSpace1,(DXGI_COLOR_SPACE_TYPE color),(color))
    EXT(4,HRESULT,SetHDRMetaData,(DXGI_HDR_METADATA_TYPE type,UINT size,void* metadata),(type,size,metadata))
#undef BASE
#undef EXT
};
}

void wrap_swap_chain(IDXGISwapChain** output) noexcept {
    if(!output || !*output)return;
    const DWORD error=GetLastError();IUnknown* identity=nullptr;
    try{
        IDXGISwapChain* real=*output;
        auto& r=registry();
        {std::lock_guard<std::mutex> lock(r.mutex);if(r.wrappers.count(real)){SetLastError(error);return;}}
        if(FAILED(real->QueryInterface(IID_PPV_ARGS(&identity)))){++tracking_failures;SetLastError(error);return;}
        DXGI_SWAP_CHAIN_DESC desc{};real->GetDesc(&desc);
        std::lock_guard<std::mutex> lock(r.mutex);
        auto found=r.objects.find(identity);
        if(found!=r.objects.end()){found->second->adopt(real);*output=found->second;}
        else{
            const auto id=r.next_id++;auto wrapper=std::make_unique<SwapChain>(real,identity,id);
            Observation o;o.id=id;o.width=desc.BufferDesc.Width;o.height=desc.BufferDesc.Height;
            o.format=desc.BufferDesc.Format;o.window=reinterpret_cast<std::uintptr_t>(desc.OutputWindow);
            r.observations.emplace(id,std::move(o));
            try{
                r.objects.emplace(identity,wrapper.get());r.wrappers.emplace(wrapper.get(),wrapper.get());
            }catch(...){r.objects.erase(identity);r.observations.erase(id);throw;}
            *output=wrapper.release();++r.created;
        }
    }catch(...){++tracking_failures;}
    if(identity)identity->Release();SetLastError(error);
}

Json swap_chain_status(){
    auto& r=registry();std::lock_guard<std::mutex> lock(r.mutex);
    Json result={{"created",r.created},{"destroyed",r.destroyed},{"tracking_failures",tracking_failures.load()},{"active",Json::array()}};
    for(const auto& pair:r.observations){const auto& o=pair.second;
        result["active"].push_back({{"id",o.id},{"width",o.width},{"height",o.height},{"format",o.format},{"window",o.window},
            {"present_calls",o.present_calls},{"present1_calls",o.present1_calls},{"test_presents",o.present_tests},
            {"successful_presents",o.presented},{"unsuccessful_presents",o.unsuccessful_presents},
            {"last_present_result",static_cast<std::uint32_t>(o.last_present)},{"resize_calls",o.resizes},
            {"failed_resizes",o.failed_resizes},{"last_resize_result",static_cast<std::uint32_t>(o.last_resize)},
            {"interface_escapes",o.escape_count},{"last_interface_escape",o.last_escape}});
    }return result;
}
}
