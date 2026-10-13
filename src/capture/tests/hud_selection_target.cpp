// Interactive application oracle: two real swap chains share one foreground HWND.
// Test signals change application presentation, never capture-runtime state.
#include "protocol.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

using Microsoft::WRL::ComPtr;
using flora::capture::Json;
namespace fs=std::filesystem;
namespace {
unsigned keys=0;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
void save(const fs::path& path,const Json& value){std::ofstream stream(path);stream<<value.dump(2);if(!stream)throw std::runtime_error("Cannot write selection evidence");}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM key,LPARAM flags){
    if(message==WM_KEYDOWN && !(flags&(LPARAM(1)<<30)) && (key==VK_F1 || key=='C'))++keys;
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,key,flags);
}
struct Surface {
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> view;
    uint64_t presents=0;
};
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    const auto out=fs::absolute(argv[1]);fs::create_directories(out);
    try{
        WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"FloraHudSelectionOracle";
        if(!RegisterClassW(&type))throw std::runtime_error("Cannot register oracle window");
        HWND window=CreateWindowW(type.lpszClassName,L"Flora HUD two swap-chain oracle",WS_OVERLAPPEDWINDOW,100,100,900,700,nullptr,nullptr,type.hInstance,nullptr);
        if(!window)throw std::runtime_error("Cannot create oracle window");
        ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGIFactory> factory;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        std::array<Surface,2> surfaces;
        const float colors[2][4]{{1,0,0,1},{0,0,1,1}};
        for(auto& surface:surfaces){
            DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=800;desc.BufferDesc.Height=600;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;
            check(factory->CreateSwapChain(device.Get(),&desc,&surface.swap));
            check(surface.swap->GetBuffer(0,IID_PPV_ARGS(&surface.buffer)));
            check(device->CreateRenderTargetView(surface.buffer.Get(),nullptr,&surface.view));
        }
        const auto deadline=GetTickCount64()+600000;bool done=false;uint64_t iterations=0;
        while(!done && GetTickCount64()<deadline && !fs::exists(out/L"finish.signal")){
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)done=true;TranslateMessage(&message);DispatchMessageW(&message);}
            if(done)break;
            const bool paused=fs::exists(out/L"pause.signal"),first=fs::exists(out/L"only-first.signal"),second=fs::exists(out/L"only-second.signal");
            for(size_t i=0;i<surfaces.size();++i){
                if(paused || (first && i==1) || (second && i==0))continue;
                auto& surface=surfaces[i];context->ClearRenderTargetView(surface.view.Get(),colors[i]);
                const auto result=surface.swap->Present(1,0);check(result);surface.presents+=result==S_OK;
            }
            if(++iterations%10==0)save(out/L"ready.json",{{"pid",GetCurrentProcessId()},{"window",reinterpret_cast<uintptr_t>(window)},
                {"keys",keys},{"presents",{surfaces[0].presents,surfaces[1].presents}},{"paused",paused},{"only_first",first},{"only_second",second}});
            Sleep(8);
        }
        Json checks=Json::array();
        for(size_t i=0;i<surfaces.size();++i){
            D3D11_TEXTURE2D_DESC desc{};surfaces[i].buffer->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&desc,nullptr,&stage));context->CopyResource(stage.Get(),surfaces[i].buffer.Get());
            D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));bool exact=true;
            for(UINT y=0;y<600;++y){const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(map.pData)+size_t(y)*map.RowPitch);for(UINT x=0;x<800;++x)exact&=row[x]==(i?0xffff0000u:0xff0000ffu);}
            context->Unmap(stage.Get(),0);checks.push_back({{"surface",i},{"exact_pixels",exact},{"presents",surfaces[i].presents}});
            if(!exact)throw std::runtime_error("HUD changed application surface pixels");
        }
        save(out/L"exit.json",{{"passed",true},{"keys",keys},{"surfaces",checks}});DestroyWindow(window);return 0;
    }catch(const std::exception& error){save(out/L"error.json",{{"error",error.what()}});std::cerr<<error.what()<<'\n';return 1;}
}
