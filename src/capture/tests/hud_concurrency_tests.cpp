#include "objects.hpp"
#include <wrl/client.h>
#include <d3d11sdklayers.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
void check(HRESULT value){if(FAILED(value))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(value)));}
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int main(int argc,char** argv){try{
    bool native_only=false,hardware=false;for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--native")native_only=true;else if(arg=="--hardware")hardware=true;else throw std::runtime_error("Unknown test argument");}
    const auto driver=hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP;
    std::atomic<bool> finished=false;
    std::jthread watchdog([&](std::stop_token stop){for(unsigned i=0;i<800 && !stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));if(!stop.stop_requested() && !finished){std::cerr<<"Timed out: application Multithread lock and presentation gate deadlocked\n"<<std::flush;ExitProcess(86);}});
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
    if(!native_only)wrap_object(context.GetAddressOf(),"HUD lock-order oracle");
    ComPtr<ID3D11Multithread> multithread;check(context.As(&multithread));multithread->SetMultithreadProtected(TRUE);require(multithread->GetMultithreadProtected(),"Native multithread protection unavailable");
    multithread->Enter();multithread->Enter();
    std::atomic<bool> started=false,returned=false;
    std::jthread worker([&]{started=true;context->Flush();returned=true;});
    while(!started)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Native semantics permit the lock owner to keep using its context while a
    // second thread waits in Flush. The wrapper must not invert those locks.
    require(context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"Owner context access changed");
    require(!returned,"Waiting context call bypassed native Enter");
    multithread->Leave();require(context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"Nested owner access failed");require(!returned,"One Leave released a nested Enter");
    multithread->Leave();worker.join();require(returned,"Waiter did not resume");
    std::cout<<"Nested protected section passed\n"<<std::flush;
    // A global gate retained across Enter would serialize independent devices
    // and deadlock otherwise-valid application barriers between their threads.
    ComPtr<ID3D11Device> other_device;ComPtr<ID3D11DeviceContext> other_context;
    check(D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&other_device,nullptr,&other_context));if(!native_only)wrap_object(other_context.GetAddressOf(),"Independent device oracle");
    ComPtr<ID3D11Multithread> other_protection;check(other_context.As(&other_protection));other_protection->SetMultithreadProtected(TRUE);
    multithread->Enter();std::atomic<bool> independent=false;
    std::jthread other([&]{other_protection->Enter();independent=other_context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;other_protection->Leave();});
    while(!independent)std::this_thread::yield();multithread->Leave();other.join();
    std::cout<<"Independent devices passed\n"<<std::flush;
    require(multithread->SetMultithreadProtected(FALSE),"Protection toggle lost prior state");
    // Disabled protection permits only externally serialized context use.
    // Keep this check on one thread; the native debug layer rejects concurrent
    // context calls in that mode even when a lower driver lock blocks Flush.
    multithread->Enter();context->Flush();require(context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE,"Disabled-protection owner access failed");multithread->Leave();
    require(!multithread->GetMultithreadProtected(),"Wrapper changed native protection flag");
    multithread->SetMultithreadProtected(TRUE);
    if(!native_only)wrap_object(device.GetAddressOf(),"HUD CPU-view oracle");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=8;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture,copy;check(device->CreateTexture2D(&desc,nullptr,&texture));check(device->CreateTexture2D(&desc,nullptr,&copy));
    for(unsigned iteration=0;iteration<8;++iteration){
        multithread->Enter();started=false;returned=false;HRESULT created=E_FAIL;
        std::jthread view_worker([&]{started=true;ComPtr<ID3D11RenderTargetView> view;created=device->CreateRenderTargetView(texture.Get(),nullptr,&view);returned=true;});
        while(!started)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // Operand unwrapping needs the alias registry while the foreign CPU
        // thread waits for native device access. It must not own that registry.
        context->CopyResource(copy.Get(),texture.Get());multithread->Leave();view_worker.join();check(created);require(returned,"View creation did not resume");
    }
    std::cout<<"Concurrent CPU view creation preserves lock-owner operand access\n"<<std::flush;
    require(object_status().value("interface_gaps",0ull)==0,"Multithread forwarding reported a gap");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;check(info->GetMessage(i,nullptr,&bytes));std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(info->GetMessage(i,message,&bytes));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(message->pDescription);}
    finished=true;watchdog.request_stop();std::cout<<"Native nested Enter/Leave preserves lock-owner progress and resumes the waiting context caller\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
