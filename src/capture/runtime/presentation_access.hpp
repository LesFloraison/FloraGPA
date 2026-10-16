#pragma once
#include <mutex>
#include <map>
#include <memory>
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <stdexcept>
namespace flora::capture {
// Only context calls and DXGI presentation hold this gate. CPU device creation
// must remain possible while Present waits for the application's message pump.
inline std::recursive_mutex& presentation_context_mutex(ID3D11Device* device){
    // Stable for the process lifetime without retaining GPU objects. Reused
    // native addresses may reuse a gate after the previous device is gone.
    struct Gates {std::mutex mutex;std::map<void*,std::unique_ptr<std::recursive_mutex>> values;};
    static auto* gates=new Gates;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if(!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))))throw std::runtime_error("Presentation device identity unavailable");
    std::lock_guard lock(gates->mutex);auto& gate=gates->values[identity.Get()];if(!gate)gate=std::make_unique<std::recursive_mutex>();return *gate;
}
inline std::recursive_mutex& presentation_context_mutex(ID3D11DeviceContext* context){Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);return presentation_context_mutex(device.Get());}
inline std::recursive_mutex& presentation_context_mutex(ID3D11VideoContext* context){Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);return presentation_context_mutex(device.Get());}
inline std::recursive_mutex& presentation_context_mutex(ID3D11Multithread* protection){
    // The runtime exposes this IID from both context and device identities.
    // A device-origin interface need not support QI to ID3D11DeviceContext.
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if(SUCCEEDED(protection->QueryInterface(IID_PPV_ARGS(&context))))return presentation_context_mutex(context.Get());
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if(FAILED(protection->QueryInterface(IID_PPV_ARGS(&device))))throw std::runtime_error("Multithread device unavailable");
    return presentation_context_mutex(device.Get());
}
inline std::recursive_mutex& presentation_context_mutex(IDXGISwapChain* swap){Microsoft::WRL::ComPtr<ID3D11Device> device;if(FAILED(swap->GetDevice(IID_PPV_ARGS(&device))))throw std::runtime_error("Presentation device unavailable");return presentation_context_mutex(device.Get());}
inline std::recursive_mutex& presentation_alias_mutex(){static auto* value=new std::recursive_mutex;return *value;}
using PresentationLock=std::lock_guard<std::recursive_mutex>;
// Only generated context proxies construct this after acquiring their device
// gate. A caller on another thread must carry this proof before a recorder may
// wait for mutable state; raw observer calls must still fail without waiting.
inline thread_local const std::recursive_mutex* current_context_gate=nullptr;
class ContextCallScope {
    const std::recursive_mutex* previous_=current_context_gate;
public:
    explicit ContextCallScope(const std::recursive_mutex* gate){current_context_gate=gate;}
    ~ContextCallScope(){current_context_gate=previous_;}
    ContextCallScope(const ContextCallScope&)=delete;
    ContextCallScope& operator=(const ContextCallScope&)=delete;
};
// Application Enter and context/presentation use the same per-device gate.
// View creation uses native protection before alias/registry locks but does not
// acquire a context gate, so CPU device work can progress while Present waits.
}
