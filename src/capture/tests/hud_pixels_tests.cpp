#include "hud_pixels.hpp"
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace flora::capture::hud;
using Microsoft::WRL::ComPtr;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Native test HRESULT: "+std::to_string(uint32_t(hr)));}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(int argc,char** argv){try{
    const bool exclusive=argc==2 && std::string(argv[1])=="--exclusive";
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudPixels";RegisterClassW(&cls);
    HWND window=CreateWindowW(cls.lpszClassName,L"Flora HUD isolation test",exclusive?WS_POPUP:WS_OVERLAPPEDWINDOW,40,40,800,600,nullptr,nullptr,cls.hInstance,nullptr);
    require(window!=nullptr,"Window creation failed");ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
    for(unsigned i=0;i<20;++i){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);}
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM})for(auto effect:{DXGI_SWAP_EFFECT_DISCARD,DXGI_SWAP_EFFECT_SEQUENTIAL}){
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=640;desc.BufferDesc.Height=480;desc.BufferDesc.Format=format;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=effect;desc.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG|D3D11_CREATE_DEVICE_SINGLETHREADED,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context));
        struct Fullscreen {IDXGISwapChain* swap;~Fullscreen(){swap->SetFullscreenState(FALSE,nullptr);}} fullscreen{swap.Get()};
        if(exclusive){
            const auto until=GetTickCount64()+30000;
            while(GetForegroundWindow()!=window && GetTickCount64()<until){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(20);}
            const auto entered=swap->SetFullscreenState(TRUE,nullptr);if(FAILED(entered))throw std::runtime_error("SetFullscreenState HRESULT: "+std::to_string(uint32_t(entered))+"; foreground="+std::to_string(GetForegroundWindow()==window));BOOL full=FALSE;check(swap->GetFullscreenState(&full,nullptr));require(full,"Exclusive fullscreen did not become active");
        }
        ComPtr<ID3D11Texture2D> buffer,stage;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC texture{};buffer->GetDesc(&texture);
        texture.BindFlags=0;texture.MiscFlags=0;texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&texture,nullptr,&stage));
        auto read=[&](){context->CopyResource(stage.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));std::vector<uint8_t> result(size_t(texture.Width)*texture.Height*4);for(UINT row=0;row<texture.Height;++row)memcpy(result.data()+size_t(row)*texture.Width*4,static_cast<uint8_t*>(m.pData)+size_t(row)*m.RowPitch,size_t(texture.Width)*4);context->Unmap(stage.Get(),0);return result;};
        std::vector<uint32_t> expected(size_t(texture.Width)*texture.Height,0xff603020u);context->UpdateSubresource(buffer.Get(),0,nullptr,expected.data(),texture.Width*4,0);
        ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view));auto* raw_view=view.Get();context->OMSetRenderTargets(1,&raw_view,nullptr);
        Panel panel;panel.adapter=L"A deliberately long adapter name for ellipsis verification";panel.device=L"DX 11.0 [HW]";panel.mode_key=L"Ctrl+F1";panel.capture_key=L"Ctrl+Shift+C";panel.fps.current=60;panel.fps.minimum=58;panel.fps.maximum=61;panel.swap=1;
        for(unsigned frame=0;frame<12;++frame){
            if(exclusive){BOOL active=FALSE;check(swap->GetFullscreenState(&active,nullptr));require(active,"Exclusive state was lost during isolation test");}
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            // Retain the HUD region; update one alternating row elsewhere.
            const UINT row=texture.Height-1-frame;std::vector<uint32_t> stripe(texture.Width,0xff000000u|frame);
            D3D11_BOX box{0,row,0,texture.Width,row+1,1};context->UpdateSubresource(buffer.Get(),0,&box,stripe.data(),texture.Width*4,0);
            const auto before=read();std::string reason;
            {PixelTransaction hud;panel.mode=frame%2?Mode::Detailed:Mode::Compact;
                SetLastError(0x71);require(hud.begin(swap.Get(),panel,0,false,reason),reason.c_str());require(GetLastError()==0x71,"HUD begin changed LastError");
                require(read()!=before,"HUD did not change displayed pixels");
                ComPtr<ID3D11RenderTargetView> actual;context->OMGetRenderTargets(1,&actual,nullptr);require(actual==view,"HUD changed output bindings");
                check(swap->Present(1,0));SetLastError(0x72);require(hud.restore(reason),reason.c_str());require(GetLastError()==0x72,"HUD restore changed LastError");
            }
            require(read()==before,"HUD remained in application-visible retained pixels");
            // Simulated failed Present: restoration without rotation must also
            // work. This is not evidence of a real driver Present failure.
            {PixelTransaction failed;require(failed.begin(swap.Get(),panel,0,false,reason),reason.c_str());require(failed.restore(reason),reason.c_str());require(read()==before,"Failed-Present restoration changed pixels");}
        }
        {PixelTransaction hud;std::string reason;const auto before=read();require(!hud.begin(swap.Get(),panel,DXGI_PRESENT_TEST,false,reason),"TEST rendered HUD");require(read()==before,"TEST polluted pixels");require(!hud.begin(swap.Get(),panel,0,true,reason),"Partial Present rendered HUD");require(read()==before,"Partial Present polluted pixels");}
        context->ClearState();view.Reset();buffer.Reset();stage.Reset();check(swap->ResizeBuffers(1,800,600,DXGI_FORMAT_UNKNOWN,desc.Flags));
        ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
        for(UINT64 index=0;index<info->GetNumStoredMessages();++index){SIZE_T size=0;check(info->GetMessage(index,nullptr,&size));std::vector<uint8_t> bytes(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(index,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::cerr<<message->pDescription<<'\n';throw std::runtime_error("HUD emitted a debug-layer warning/error");}}
    }
    DestroyWindow(window);std::cout<<"Single-buffer discard/sequential: 24 retained frames, state, TEST, partial, resize and debug layer passed; exclusive="<<exclusive<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
