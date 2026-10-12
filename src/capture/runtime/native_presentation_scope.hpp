#pragma once
#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>
namespace flora::capture {
// CPU view operations take native protection before alias/registry locks,
// without acquiring the presentation context gate. This lets an application
// Enter owner resolve operands while another thread waits in view creation.
// Never change the application's protection settings.
class NativePresentationScope {
    Microsoft::WRL::ComPtr<ID3D11Multithread> protection_;
    void enter(ID3D11DeviceContext* context){
        if(context && SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&protection_))))protection_->Enter();
    }
    void enter(ID3D11Device* device){Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;if(device)device->GetImmediateContext(&context);enter(context.Get());}
public:
    explicit NativePresentationScope(ID3D11DeviceContext* context){const auto error=GetLastError();Microsoft::WRL::ComPtr<ID3D11Device> device;context->GetDevice(&device);enter(device.Get());SetLastError(error);}
    explicit NativePresentationScope(ID3D11Device* device){const auto error=GetLastError();enter(device);SetLastError(error);}
    explicit NativePresentationScope(ID3D11DeviceChild* child){const auto error=GetLastError();Microsoft::WRL::ComPtr<ID3D11Device> device;child->GetDevice(&device);enter(device.Get());SetLastError(error);}
    explicit NativePresentationScope(IDXGIDeviceSubObject* object){const auto error=GetLastError();Microsoft::WRL::ComPtr<ID3D11Device> device;object->GetDevice(IID_PPV_ARGS(&device));enter(device.Get());SetLastError(error);}
    ~NativePresentationScope(){const auto error=GetLastError();if(protection_)protection_->Leave();protection_.Reset();SetLastError(error);}
    NativePresentationScope(const NativePresentationScope&)=delete;
    NativePresentationScope& operator=(const NativePresentationScope&)=delete;
};
}
