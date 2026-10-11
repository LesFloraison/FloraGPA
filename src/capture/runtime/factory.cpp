#include "factory.hpp"
#include "swap_chain.hpp"
#include "objects.hpp"
#include <detours.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>

namespace flora::capture {
namespace {
class Factory;
struct Registry {
    std::mutex mutex;
    std::map<IUnknown *, Factory *> objects;
    std::map<IDXGIFactory *, Factory *> wrappers;
    uint64_t created = 0, destroyed = 0;
};
Registry &registry() { static auto *r = new Registry; return *r; }
std::atomic<unsigned> failures{0}, escapes{0};
bool known(REFIID iid) {
    return iid == __uuidof(IUnknown) || iid == __uuidof(IDXGIObject) ||
           iid == __uuidof(IDXGIFactory) || iid == __uuidof(IDXGIFactory1) ||
           iid == __uuidof(IDXGIFactory2) || iid == __uuidof(IDXGIFactory3) ||
           iid == __uuidof(IDXGIFactory4) || iid == __uuidof(IDXGIFactory5) ||
           iid == __uuidof(IDXGIFactory6) || iid == __uuidof(IDXGIFactory7);
}
template<class T> HRESULT observed(HRESULT hr, T **output, const char* api) {
    if (SUCCEEDED(hr)) wrap_object(output, api);
    return hr;
}
template<class T> HRESULT wrapped(HRESULT hr, T **output) {
    if (SUCCEEDED(hr) && output && *output) {
        const DWORD error = GetLastError();
        IDXGISwapChain *base = nullptr;
        if (SUCCEEDED((*output)->QueryInterface(IID_PPV_ARGS(&base)))) {
            wrap_swap_chain(&base);
            T *result = nullptr;
            if (SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&result)))) {
                (*output)->Release(); *output = result;
            } else ++failures;
            base->Release();
        } else ++failures;
        SetLastError(error);
    }
    return hr;
}
class Factory final : public IDXGIFactory7 {
    IDXGIFactory *real_;
    IUnknown *identity_;
    ULONG refs_ = 1;
    template<class T, class F> auto extended(F action) -> decltype(action(static_cast<T *>(nullptr))) {
        using Result = decltype(action(static_cast<T *>(nullptr)));
        const DWORD before = GetLastError();
        T *view = nullptr;
        const HRESULT hr = real_->QueryInterface(IID_PPV_ARGS(&view));
        if (FAILED(hr)) {
            if constexpr (std::is_void_v<Result>) return;
            else return static_cast<Result>(hr);
        }
        SetLastError(before);
        if constexpr (std::is_void_v<Result>) {
            action(view); const DWORD after = GetLastError(); view->Release(); SetLastError(after);
        } else {
            auto result = action(view); const DWORD after = GetLastError(); view->Release(); SetLastError(after); return result;
        }
    }
  public:
    Factory(IDXGIFactory *real, IUnknown *identity) : real_(real), identity_(identity) {}
    void adopt(IDXGIFactory *returned) { ++refs_; real_->AddRef(); returned->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **output) override {
        auto hr = real_->QueryInterface(iid, output); const DWORD error = GetLastError();
        if (SUCCEEDED(hr) && output && *output) {
            if (known(iid)) {
                AddRef(); static_cast<IUnknown *>(*output)->Release(); *output = static_cast<IDXGIFactory7 *>(this);
            } else { ++escapes; object_gap("IDXGIFactory::QueryInterface", iid, "Interface forwarding is not implemented"); }
        }
        SetLastError(error); return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        const DWORD before = GetLastError();
        { auto &r = registry(); std::lock_guard lock(r.mutex); ++refs_; }
        SetLastError(before); return real_->AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const DWORD before = GetLastError(); bool destroy = false; auto *real = real_;
        { auto &r = registry(); std::lock_guard lock(r.mutex);
          if (--refs_ == 0) { r.objects.erase(identity_); r.wrappers.erase(this); ++r.destroyed; destroy = true; } }
        SetLastError(before); const ULONG count = real->Release(); const DWORD after = GetLastError();
        if (destroy) delete this; SetLastError(after); return count;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) override {
        NativeCall internal;
        return wrapped(real_->CreateSwapChain(unwrap(device), desc, out), out);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGIOutput *restrictOutput, IDXGISwapChain1 **out) override {
        NativeCall internal;
        return wrapped(extended<IDXGIFactory2>([&](auto *p) { return p->CreateSwapChainForHwnd(unwrap(device), window, desc, fullscreen, unwrap(restrictOutput), out); }), out);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        IDXGIOutput *restrictOutput, IDXGISwapChain1 **out) override {
        NativeCall internal;
        return wrapped(extended<IDXGIFactory2>([&](auto *p) { return p->CreateSwapChainForCoreWindow(unwrap(device), window, desc, unwrap(restrictOutput), out); }), out);
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc,
        IDXGIOutput *restrictOutput, IDXGISwapChain1 **out) override {
        NativeCall internal;
        return wrapped(extended<IDXGIFactory2>([&](auto *p) { return p->CreateSwapChainForComposition(unwrap(device), desc, unwrap(restrictOutput), out); }), out);
    }
#define BASE(result,name,args,call) result STDMETHODCALLTYPE name args override { return real_->name call; }
#define EXT(v,result,name,args,call) result STDMETHODCALLTYPE name args override { return extended<IDXGIFactory##v>([&](auto *p) { return p->name call; }); }
#define ESCBASE(name,args,call,out) HRESULT STDMETHODCALLTYPE name args override { return observed(real_->name call, out, "IDXGIFactory::" #name); }
#define ESCEXT(v,name,args,call,out) HRESULT STDMETHODCALLTYPE name args override { return observed(extended<IDXGIFactory##v>([&](auto *p) { return p->name call; }),out,"IDXGIFactory::" #name); }
#define IIDEXT(v,name,args,call) HRESULT STDMETHODCALLTYPE name args override { auto hr = extended<IDXGIFactory##v>([&](auto *p) { return p->name call; }); if(SUCCEEDED(hr)) wrap_object(iid,out,"IDXGIFactory::" #name); return hr; }
    BASE(HRESULT,SetPrivateData,(REFGUID name,UINT size,const void *data),(name,size,data))
    BASE(HRESULT,SetPrivateDataInterface,(REFGUID name,const IUnknown *object),(name,object))
    BASE(HRESULT,GetPrivateData,(REFGUID name,UINT *size,void *data),(name,size,data))
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid, void** out) override { auto hr=real_->GetParent(iid,out);if(SUCCEEDED(hr))wrap_object(iid,out,"IDXGIFactory::GetParent");return hr; }
    ESCBASE(EnumAdapters,(UINT index,IDXGIAdapter **out),(index,out),out)
    ESCBASE(CreateSoftwareAdapter,(HMODULE module,IDXGIAdapter **out),(module,out),out)
    BASE(HRESULT,MakeWindowAssociation,(HWND window,UINT flags),(window,flags))
    BASE(HRESULT,GetWindowAssociation,(HWND *window),(window))
    ESCEXT(1,EnumAdapters1,(UINT index,IDXGIAdapter1 **out),(index,out),out)
    EXT(1,BOOL,IsCurrent,(),())
    EXT(2,BOOL,IsWindowedStereoEnabled,(),())
    EXT(2,HRESULT,GetSharedResourceAdapterLuid,(HANDLE resource,LUID *luid),(resource,luid))
    EXT(2,HRESULT,RegisterStereoStatusWindow,(HWND window,UINT message,DWORD *cookie),(window,message,cookie))
    EXT(2,HRESULT,RegisterStereoStatusEvent,(HANDLE event,DWORD *cookie),(event,cookie))
    EXT(2,void,UnregisterStereoStatus,(DWORD cookie),(cookie))
    EXT(2,HRESULT,RegisterOcclusionStatusWindow,(HWND window,UINT message,DWORD *cookie),(window,message,cookie))
    EXT(2,HRESULT,RegisterOcclusionStatusEvent,(HANDLE event,DWORD *cookie),(event,cookie))
    EXT(2,void,UnregisterOcclusionStatus,(DWORD cookie),(cookie))
    EXT(3,UINT,GetCreationFlags,(),())
    IIDEXT(4,EnumAdapterByLuid,(LUID luid,REFIID iid,void **out),(luid,iid,out))
    IIDEXT(4,EnumWarpAdapter,(REFIID iid,void **out),(iid,out))
    EXT(5,HRESULT,CheckFeatureSupport,(DXGI_FEATURE feature,void *data,UINT size),(feature,data,size))
    IIDEXT(6,EnumAdapterByGpuPreference,(UINT index,DXGI_GPU_PREFERENCE pref,REFIID iid,void **out),(index,pref,iid,out))
    EXT(7,HRESULT,RegisterAdaptersChangedEvent,(HANDLE event,DWORD *cookie),(event,cookie))
    EXT(7,HRESULT,UnregisterAdaptersChangedEvent,(DWORD cookie),(cookie))
#undef BASE
#undef EXT
#undef ESCBASE
#undef ESCEXT
#undef IIDEXT
};
decltype(&CreateDXGIFactory) original0 = CreateDXGIFactory;
decltype(&CreateDXGIFactory1) original1 = CreateDXGIFactory1;
decltype(&CreateDXGIFactory2) original2 = CreateDXGIFactory2;
thread_local unsigned creationDepth = 0;
HRESULT WINAPI create0(REFIID iid, void **out) {
    if(native_call_depth)return original0(iid,out);
    ++creationDepth; auto hr = original0(iid, out);
    if (--creationDepth == 0 && SUCCEEDED(hr)) wrap_factory(iid, out);
    return hr;
}
HRESULT WINAPI create1(REFIID iid, void **out) {
    if(native_call_depth)return original1(iid,out);
    ++creationDepth; auto hr = original1(iid, out);
    if (--creationDepth == 0 && SUCCEEDED(hr)) wrap_factory(iid, out);
    return hr;
}
HRESULT WINAPI create2(UINT flags, REFIID iid, void **out) {
    if(native_call_depth)return original2(flags,iid,out);
    ++creationDepth; auto hr = original2(flags, iid, out);
    if (--creationDepth == 0 && SUCCEEDED(hr)) wrap_factory(iid, out);
    return hr;
}
}
void wrap_factory(REFIID iid, void **out) noexcept {
    if (!out || !*out) return;
    if (!known(iid)) { ++escapes; object_gap("CreateDXGIFactory/GetParent",iid,"Factory interface forwarding is not implemented"); return; }
    const DWORD error = GetLastError(); IDXGIFactory *base = nullptr; IUnknown *identity = nullptr;
    try {
        auto &r = registry();
        { std::lock_guard lock(r.mutex); if (r.wrappers.contains(static_cast<IDXGIFactory *>(*out))) { SetLastError(error); return; } }
        auto *returned = static_cast<IUnknown *>(*out);
        if (FAILED(returned->QueryInterface(IID_PPV_ARGS(&base))) || FAILED(returned->QueryInterface(IID_PPV_ARGS(&identity))))
            throw std::runtime_error("Factory identity unavailable");
        {
            std::lock_guard lock(r.mutex);
            const auto found = r.objects.find(identity);
            if (found != r.objects.end()) { found->second->adopt(base); *out = static_cast<IDXGIFactory7 *>(found->second); }
            else {
                auto wrapper = std::make_unique<Factory>(base, identity);
                r.objects.emplace(identity, wrapper.get());
                try { r.wrappers.emplace(wrapper.get(), wrapper.get()); }
                catch (...) { r.objects.erase(identity); throw; }
                *out = static_cast<IDXGIFactory7 *>(wrapper.release()); ++r.created;
            }
        }
        // Each wrapper reference owns a base reference. No native call under lock.
        base = nullptr; returned->Release();
    } catch (...) { ++failures; }
    if (base) base->Release();
    if (identity) identity->Release();
    SetLastError(error);
}
Json factory_status() {
    auto &r = registry(); std::lock_guard lock(r.mutex);
    return {{"created",r.created},{"destroyed",r.destroyed},{"active",r.objects.size()},
            {"tracking_failures",failures.load()},{"interface_escapes",escapes.load()}};
}
LONG attach_factory_hooks() {
    auto error = DetourAttach(reinterpret_cast<PVOID *>(&original0), create0);
    if (error == NO_ERROR) error = DetourAttach(reinterpret_cast<PVOID *>(&original1), create1);
    if (error == NO_ERROR) error = DetourAttach(reinterpret_cast<PVOID *>(&original2), create2);
    return error;
}
}
