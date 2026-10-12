// Native-only presentation experiment. Not linked into the injected runtime.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Native probe HRESULT: "+std::to_string(uint32_t(hr)));}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(){try{
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudBufferProbe";RegisterClassW(&cls);
    auto window=CreateWindowW(cls.lpszClassName,L"Flora HUD buffer experiment",WS_OVERLAPPEDWINDOW,100,100,160,160,nullptr,nullptr,cls.hInstance,nullptr);ShowWindow(window,SW_SHOWNOACTIVATE);
    Json results=Json::array();
    for(const auto effect:{DXGI_SWAP_EFFECT_DISCARD,DXGI_SWAP_EFFECT_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_DISCARD})for(bool restore:{false,true}){
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=desc.BufferDesc.Height=64;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SampleDesc.Count=1;desc.BufferCount=effect==DXGI_SWAP_EFFECT_DISCARD?1:3;desc.Windowed=TRUE;desc.OutputWindow=window;desc.SwapEffect=effect;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context));
        ComPtr<ID3D11Texture2D> buffer,backup,stage;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC d{};buffer->GetDesc(&d);d.BindFlags=0;d.MiscFlags=0;check(device->CreateTexture2D(&d,nullptr,&backup));d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&d,nullptr,&stage));
        ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));ComPtr<IDXGISwapChain3> swap3;swap.As(&swap3);
        auto pixels=[&](ID3D11Texture2D* source){context->CopyResource(stage.Get(),source);D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));auto v=*static_cast<uint32_t*>(m.pData);context->Unmap(stage.Get(),0);return v;};
        Json row={{"effect",effect},{"restore",restore},{"frames",Json::array()}};
        for(unsigned i=0;i<8;++i){
            const float color[]{float(i+1)/16,0,0,1};context->ClearRenderTargetView(rtv.Get(),color);const auto before=pixels(buffer.Get());const auto index=swap3?swap3->GetCurrentBackBufferIndex():0;
            context->CopyResource(backup.Get(),buffer.Get());
            if(restore){const float hud[]{0,1,0,1};context->ClearRenderTargetView(rtv.Get(),hud);}
            const auto hr=swap->Present(1,0);check(hr);
            ComPtr<ID3D11Texture2D> after;check(swap->GetBuffer(0,IID_PPV_ARGS(&after)));
            Json slots=Json::array();
            for(UINT slot=0;slot<desc.BufferCount;++slot){ComPtr<ID3D11Texture2D> other;const auto got=swap->GetBuffer(slot,IID_PPV_ARGS(&other));if(SUCCEEDED(got)){const auto value=pixels(other.Get());if(restore && value==0xff00ff00u)context->CopyResource(other.Get(),backup.Get());slots.push_back({{"index",slot},{"before",value},{"after",pixels(other.Get())}});}else slots.push_back({{"index",slot},{"hr",uint32_t(got)}});}
            const auto presented=pixels(after.Get());
            row["frames"].push_back({{"slots",slots},{"before",before},{"after_present",presented},{"after_restore",pixels(after.Get())},{"same_identity",buffer==after},{"index_before",index},{"index_after",swap3?swap3->GetCurrentBackBufferIndex():0},{"result",uint32_t(hr)}});
        }
        results.push_back(row);context->ClearState();context->Flush();
    }
    DestroyWindow(window);std::cout<<results.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
