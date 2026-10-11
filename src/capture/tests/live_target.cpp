// Self-owned rendering target. No capture/replay runtime linkage or direct hook calls.
#include "protocol.hpp"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
// Windows can report ACCESS_DENIED while the peer's signal deletion is pending.
// Retry that transient observation; do not restart the target or alter outcomes.
bool signal_exists(const fs::path& path){
    for(unsigned attempt=0;attempt<100;++attempt){std::error_code error;const bool found=fs::exists(path,error);
        if(!error)return found;
        if(error.value()!=ERROR_ACCESS_DENIED && error.value()!=ERROR_SHARING_VIOLATION)throw fs::filesystem_error("Observe target signal",path,error);
        Sleep(5);
    }throw std::runtime_error("Target signal observation remained inaccessible");
}

void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Live target DX11 call failed");}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
void save(const fs::path& path,const Json& value){auto temp=path;temp+=L".tmp";{std::ofstream file(temp);file<<value.dump(2);if(!file)throw std::runtime_error("Target evidence write failed");}fs::rename(temp,path);}
}
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;const fs::path out=fs::absolute(argv[1]);fs::create_directories(out);try{
    const UINT mode=std::stoul(argv[2]);
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraLiveCaptureTarget";RegisterClassW(&cls);
    HWND window=CreateWindowW(cls.lpszClassName,L"Flora native capture target",WS_OVERLAPPEDWINDOW,0,0,160,160,nullptr,nullptr,cls.hInstance,nullptr);if(!window)throw std::runtime_error("Target window creation failed");ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
    DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=d.BufferDesc.Height=8;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=1;d.OutputWindow=window;d.Windowed=TRUE;
    if(mode){check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<IDXGIFactory> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));check(factory->CreateSwapChain(device.Get(),&d,&swap));}
    else check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&device,nullptr,&context));
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);
    const auto shader=[&](const char* text,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(text,strlen(text),"live-capture",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;};
    auto code=shader("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
    code=shader("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);
    ComPtr<IDXGISwapChain1> swap1;check(swap.As(&swap1));uint64_t frames=0;const auto deadline=GetTickCount64()+120000;
    while(!signal_exists(out/L"finish.signal")){
        if(GetTickCount64()>deadline)throw std::runtime_error("Live target control timeout");
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(signal_exists(out/L"pause.signal")){if(!signal_exists(out/L"paused.signal"))save(out/L"paused.signal",{{"frames",frames}});Sleep(5);continue;}
        if(signal_exists(out/L"paused.signal"))fs::remove(out/L"paused.signal");
        const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);context->Draw(3,0);
        if(signal_exists(out/L"update.signal")){UINT pixels[64]{};context->UpdateSubresource(buffer.Get(),0,nullptr,pixels,32,256);}
        if(mode){DXGI_PRESENT_PARAMETERS parameters{};check(swap1->Present1(0,0,&parameters));}else check(swap->Present(0,0));
        ++frames;if(frames==3)save(out/L"ready.json",{{"frames",frames},{"mode",mode},{"window",reinterpret_cast<uintptr_t>(window)}});
        Sleep(10);
    }
    context->ClearState();context->Flush();save(out/L"exit.json",{{"frames",frames},{"ok",true}});DestroyWindow(window);return 0;
}catch(const std::exception& e){save(out/L"error.json",{{"error",e.what()}});std::cerr<<e.what()<<'\n';return 1;}}
