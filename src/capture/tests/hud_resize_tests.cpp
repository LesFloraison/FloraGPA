// ResizeBuffers1 is specified for D3D12 queues. Compare the native DX11
// rejection with our wrapper, including clean-storage reattachment afterward.
#include "hud_control.hpp"
#include "objects.hpp"
#include "swap_chain.hpp"
#include <d3d11sdklayers.h>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::vector<uint8_t> pixels(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source){
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;check(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),source);
    D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<uint8_t> result(size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y)memcpy(result.data()+size_t(y)*desc.Width*4,static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch,desc.Width*4);
    context->Unmap(staging.Get(),0);return result;
}
std::vector<int> diagnostics(ID3D11InfoQueue* info){
    std::vector<int> result;
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> data(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());check(info->GetMessage(i,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)result.push_back(int(message->ID));}
    return result;
}
struct Result {HRESULT held,unheld;DWORD held_error,unheld_error;std::vector<int> held_messages,unheld_messages;};
Result run(HWND window,bool hardware,bool wrapped,DXGI_SWAP_EFFECT effect){
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=desc.BufferDesc.Height=64;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=effect;desc.Windowed=TRUE;desc.OutputWindow=window;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
    check(D3D11CreateDeviceAndSwapChain(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
    ComPtr<ID3D11Texture2D> physical;check(swap->GetBuffer(0,IID_PPV_ARGS(&physical)));
    if(wrapped){wrap_object(device.GetAddressOf(),"Resize oracle device");wrap_object(context.GetAddressOf(),"Resize oracle context");wrap_swap_chain(swap.GetAddressOf());}
    ComPtr<IDXGISwapChain3> swap3;check(swap.As(&swap3));
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));
    ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view));
    const float color[]{32.f/255,48.f/255,96.f/255,1};context->ClearRenderTargetView(view.Get(),color);
    const auto before=pixels(device.Get(),context.Get(),buffer.Get());
    if(wrapped)require(unwrap(buffer.Get())!=physical.Get(),"Expected private clean presentation storage");
    require(diagnostics(info.Get()).empty(),"Unexpected initialization diagnostics");info->ClearStoredMessages();
    Result result{};SetLastError(0x51a7);result.held=swap3->ResizeBuffers1(2,96,96,DXGI_FORMAT_UNKNOWN,0,nullptr,nullptr);result.held_error=GetLastError();result.held_messages=diagnostics(info.Get());info->ClearStoredMessages();
    require(FAILED(result.held),"DX11 ResizeBuffers1 unexpectedly succeeded with held buffers");
    require(pixels(device.Get(),context.Get(),buffer.Get())==before,"Rejected ResizeBuffers1 changed retained pixels");
    if(wrapped)require(unwrap(buffer.Get())!=physical.Get(),"Rejected resize failed to reattach clean storage");
    ComPtr<ID3D11Texture2D> again;check(swap->GetBuffer(0,IID_PPV_ARGS(&again)));require(again==buffer,"Rejected resize changed resource identity");again.Reset();
    context->ClearState();view.Reset();buffer.Reset();physical.Reset();context->Flush();
    require(diagnostics(info.Get()).empty(),"Unexpected post-rejection diagnostics");info->ClearStoredMessages();
    SetLastError(0x51a7);result.unheld=swap3->ResizeBuffers1(2,96,96,DXGI_FORMAT_UNKNOWN,0,nullptr,nullptr);result.unheld_error=GetLastError();result.unheld_messages=diagnostics(info.Get());info->ClearStoredMessages();
    require(FAILED(result.unheld),"DX11 ResizeBuffers1 unexpectedly succeeded without D3D12 queues");
    check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));require(pixels(device.Get(),context.Get(),buffer.Get())==before,"Unheld rejected resize lost clean storage");buffer.Reset();
    check(swap->ResizeBuffers(2,96,96,DXGI_FORMAT_UNKNOWN,0));
    check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view));context->ClearRenderTargetView(view.Get(),color);
    D3D11_TEXTURE2D_DESC resized{};buffer->GetDesc(&resized);require(resized.Width==96 && resized.Height==96,"Ordinary resize failed to rebuild buffers");
    const auto after=pixels(device.Get(),context.Get(),buffer.Get());for(size_t i=0;i<after.size();i+=4)require(after[i]==32 && after[i+1]==48 && after[i+2]==96 && after[i+3]==255,"New-generation resource contents differ");
    require(diagnostics(info.Get()).empty(),"Unexpected successful-resize diagnostics");
    context->ClearState();return result;
}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(int argc,char**){try{
    hud::Settings settings;settings.mode_key={};settings.capture_key={};configure_hud(settings);
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudResizeOracle";require(RegisterClassW(&cls)!=0,"Window class failed");
    for(const auto effect:{DXGI_SWAP_EFFECT_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL}){
        const auto execute=[&](bool wrapped){HWND window=CreateWindowW(cls.lpszClassName,L"Flora HUD resize oracle",WS_POPUP,0,0,128,128,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Window failed");const auto result=run(window,argc>1,wrapped,effect);DestroyWindow(window);return result;};
        const auto native=execute(false),wrapped=execute(true);
        require(native.held==wrapped.held && native.unheld==wrapped.unheld,"ResizeBuffers1 HRESULT differs from native");
        require(native.held_error==wrapped.held_error && native.unheld_error==wrapped.unheld_error,"ResizeBuffers1 LastError differs from native");
        require(native.held_messages==wrapped.held_messages && native.unheld_messages==wrapped.unheld_messages,"ResizeBuffers1 debug diagnostics differ from native");
        require(object_status().value("interface_gaps",0ull)==0,"ResizeBuffers1 left a capability gap");
        require(object_status()["active"]==0 && metadata_status()["alive"]==0,"Resize cleanup retained application objects");
        std::cout<<"effect="<<effect<<" held="<<uint32_t(native.held)<<" unheld="<<uint32_t(native.unheld)<<" LastError="<<native.held_error<<","<<native.unheld_error<<" exact pixels, identity, diagnostics and subsequent ResizeBuffers passed\n";
    }
    disconnect_hud();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
