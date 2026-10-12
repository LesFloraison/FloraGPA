// Independent application oracle: intentionally retains all pixels beneath HUD.
#include "protocol.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
void check(HRESULT h){if(FAILED(h))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(h)));}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){if(m==WM_CLOSE){DestroyWindow(w);return 0;}if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(w,m,a,b);}
int wmain(int argc,wchar_t** argv){if(argc<2 || argc>3)return 2;const fs::path out=fs::absolute(argv[1]);const std::wstring mode=argc==3?argv[2]:L"single";fs::create_directories(out);try{
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudLiveOracle";RegisterClassW(&cls);
    RECT bounds{100,100,1000,800};if(mode==L"borderless"){MONITORINFO monitor{sizeof(MONITORINFO)};if(!GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&monitor))throw std::runtime_error("Cannot query borderless monitor");bounds=monitor.rcMonitor;}
    HWND window=CreateWindowW(cls.lpszClassName,L"Flora HUD retained scene",mode==L"borderless"?WS_POPUP:WS_OVERLAPPEDWINDOW,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,cls.hInstance,nullptr);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
    DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=800;d.BufferDesc.Height=600;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SampleDesc.Count=1;d.BufferCount=1;d.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;d.OutputWindow=window;d.Windowed=TRUE;
    if(mode!=L"single"){d.BufferCount=mode==L"flip3"?3:2;d.SwapEffect=mode==L"blt"?DXGI_SWAP_EFFECT_SEQUENTIAL:DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;}
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
    check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,mode==L"single"?D3D11_CREATE_DEVICE_SINGLETHREADED:0,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&device,nullptr,&context));
    if(mode==L"exclusive"){check(swap->SetFullscreenState(TRUE,nullptr));check(swap->ResizeBuffers(d.BufferCount,800,600,DXGI_FORMAT_UNKNOWN,0));BOOL fullscreen=FALSE;check(swap->GetFullscreenState(&fullscreen,nullptr));if(!fullscreen)throw std::runtime_error("Exclusive fullscreen was not entered");}
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));const float color[]{32.f/255,48.f/255,96.f/255,1};context->ClearRenderTargetView(rtv.Get(),color);
    for(UINT i=0;i<d.BufferCount;++i){context->ClearRenderTargetView(rtv.Get(),color);if(swap->Present(1,0)!=S_OK)throw std::runtime_error("Warmup Present was not successful");}
    const auto compile=[](const char* source,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(source,strlen(source),"hud-application-oracle",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;};
    auto code=compile("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    code=compile("float4 main():SV_Target{return float4(1,1,1,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT viewport{799,599,1,1,0,1};context->RSSetViewports(1,&viewport);
    {std::ofstream ready(out/L"ready.json");ready<<flora::capture::Json{{"window",reinterpret_cast<uintptr_t>(window)},{"pid",GetCurrentProcessId()}}.dump();}
    uint64_t frames=0;bool done=false,exercised=false;const auto deadline=GetTickCount64()+600000;
    while(!done && !fs::exists(out/L"finish.signal") && GetTickCount64()<deadline){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)done=true;TranslateMessage(&msg);DispatchMessageW(&msg);}if(done)break;
        if(!exercised && fs::exists(out/L"exercise.signal")){
            const auto clean_region=[&]{
                D3D11_TEXTURE2D_DESC td{};buffer->GetDesc(&td);td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&td,nullptr,&stage));context->CopyResource(stage.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped));bool clean=true;
                for(UINT y=0;y<600;++y){const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch);for(UINT x=0;x<800;++x)if((x!=799 || y!=599) && row[x]!=0xff603020u)clean=false;}context->Unmap(stage.Get(),0);if(!clean)throw std::runtime_error("Present/Resize polluted retained application pixels");
            };
            const auto held=swap->ResizeBuffers(0,800,600,DXGI_FORMAT_UNKNOWN,0);if(held!=DXGI_ERROR_INVALID_CALL)throw std::runtime_error("Retained application references did not reject Resize");
            clean_region();
            if(swap->Present(0,DXGI_PRESENT_TEST)!=S_OK)throw std::runtime_error("TEST Present failed");
            clean_region();
            const auto failed=swap->Present(5,0);if(failed!=DXGI_ERROR_INVALID_CALL)throw std::runtime_error("Invalid Present interval did not fail");
            clean_region();
            ComPtr<IDXGISwapChain1> swap1;check(swap.As(&swap1));RECT dirty{799,599,800,600};DXGI_PRESENT_PARAMETERS parameters{};parameters.DirtyRectsCount=1;parameters.pDirtyRects=&dirty;
            if(swap1->Present1(1,0,&parameters)!=S_OK)throw std::runtime_error("Partial Present1 failed");
            clean_region();
            context->OMSetRenderTargets(0,nullptr,nullptr);rtv.Reset();buffer.Reset();context->Flush();
            check(swap->ResizeBuffers(0,800,600,DXGI_FORMAT_UNKNOWN,0));
            check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));view=rtv.Get();
            for(UINT i=0;i<d.BufferCount;++i){context->ClearRenderTargetView(rtv.Get(),color);if(swap->Present(1,0)!=S_OK)throw std::runtime_error("Resized warmup Present failed");}
            exercised=true;std::ofstream result(out/L"exercise.json");result<<flora::capture::Json{{"held_resize_rejected",true},{"failed_present_restored",true},{"test_present",true},{"partial_present1",true},{"same_size_resize",true}}.dump(2);
        }
        const UINT black=0xff000000;D3D11_BOX box{799,599,0,800,600,1};context->UpdateSubresource(buffer.Get(),0,&box,&black,4,4);context->OMSetRenderTargets(1,&view,nullptr);context->Draw(3,0);
        if(mode==L"exclusive"){BOOL fullscreen=FALSE;check(swap->GetFullscreenState(&fullscreen,nullptr));if(!fullscreen)throw std::runtime_error("Exclusive fullscreen was lost");}
        const auto presented=swap->Present(1,0);check(presented);if(presented==S_OK)++frames;Sleep(8);
    }
    // This read occurs only after the controller has stopped capturing.
    D3D11_TEXTURE2D_DESC td{};buffer->GetDesc(&td);td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&td,nullptr,&stage));context->CopyResource(stage.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));bool exact=true;
    std::ofstream pixels(out/L"scene.rgba",std::ios::binary);for(UINT y=0;y<600;++y){auto* row=static_cast<uint32_t*>(static_cast<void*>(static_cast<uint8_t*>(m.pData)+size_t(y)*m.RowPitch));for(UINT x=0;x<800;++x)if(row[x]!=(x==799 && y==599?0xffffffffu:0xff603020u))exact=false;pixels.write(reinterpret_cast<const char*>(row),3200);}context->Unmap(stage.Get(),0);
    {std::ofstream result(out/L"exit.json");result<<flora::capture::Json{{"frames",frames},{"exact_retained_pixels",exact},{"mode",std::string(mode.begin(),mode.end())}}.dump(2);}if(mode==L"exclusive")check(swap->SetFullscreenState(FALSE,nullptr));DestroyWindow(window);return exact?0:1;
}catch(const std::exception& e){std::ofstream report(out/L"error.json");report<<flora::capture::Json{{"error",e.what()}}.dump();std::cerr<<e.what()<<'\n';return 1;}}
