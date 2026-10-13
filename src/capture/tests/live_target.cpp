// Self-owned rendering target. No capture/replay runtime linkage or direct hook calls.
#include "protocol.hpp"
#include <d3d11_3.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include "msaa_live_renderer.hpp"
#include <memory>
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
    const bool msaa=mode==24 || mode==25;
    if(msaa){d.BufferDesc.Width=MsaaLiveRenderer::width*(mode==24?2:4);d.BufferDesc.Height=MsaaLiveRenderer::height*MsaaLiveRenderer::layers;}
    const bool tearing=mode==9 || mode==10 || mode==11 || mode==12;const bool flip=mode>=2 && mode!=8 && mode!=24;
    if(flip){d.BufferCount=(mode==6 || mode==7)?16:((mode==3 || mode==5 || mode==10)?3:2);d.SwapEffect=(mode==2 || mode==3 || mode==6 || mode==9)?DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:DXGI_SWAP_EFFECT_FLIP_DISCARD;}
    if(tearing){ComPtr<IDXGIFactory5> features;check(CreateDXGIFactory1(IID_PPV_ARGS(&features)));BOOL supported=FALSE;check(features->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported)));if(!supported)throw std::runtime_error("Tearing is unsupported on this host");d.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;}
    if(mode==8){d.BufferCount=3;d.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;}
    if(mode==3 || mode==5 || mode==6 || mode==7 || mode==9 || mode==12 || mode==25){
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<IDXGIFactory2> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 modern{};modern.Width=d.BufferDesc.Width;modern.Height=d.BufferDesc.Height;modern.Format=d.BufferDesc.Format;modern.SampleDesc=d.SampleDesc;modern.BufferUsage=d.BufferUsage;modern.BufferCount=d.BufferCount;modern.SwapEffect=d.SwapEffect;modern.AlphaMode=DXGI_ALPHA_MODE_IGNORE;modern.Flags=d.Flags;
        ComPtr<IDXGISwapChain1> created;check(factory->CreateSwapChainForHwnd(device.Get(),window,&modern,nullptr,nullptr,&created));check(created.As(&swap));
    }else if(mode==1 || mode==8){check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));ComPtr<IDXGIFactory> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));check(factory->CreateSwapChain(device.Get(),&d,&swap));}
    else check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&device,nullptr,&context));
    ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));ComPtr<ID3D11RenderTargetView> rtv;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);
    const auto shader=[&](const char* text,const char* profile){ComPtr<ID3DBlob> code;check(D3DCompile(text,strlen(text),"live-capture",nullptr,nullptr,"main",profile,0,0,&code,nullptr));return code;};
    auto code=shader("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
    code=shader("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);
    const std::array<const char*,5> colors{"1,0,0,1","0,1,0,1","0,0,1,1","1,1,1,1","0,1,1,1"};std::array<ComPtr<ID3D11PixelShader>,5> cycle;
    for(size_t i=0;i<colors.size();++i){const auto source=std::string("float4 main():SV_Target{return float4(")+colors[i]+");}";auto bytes=shader(source.c_str(),"ps_5_0");check(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&cycle[i]));}
    ComPtr<ID3D11Buffer> mapped_color;ComPtr<ID3D11PixelShader> mapped_shader;
    if(mode>=11 && mode<=14){D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.Usage=mode>=13?D3D11_USAGE_DEFAULT:D3D11_USAGE_DYNAMIC;bd.CPUAccessFlags=mode>=13?0:D3D11_CPU_ACCESS_WRITE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;const float blue[]{0,0,1,1};D3D11_SUBRESOURCE_DATA initial{blue,0,0};check(device->CreateBuffer(&bd,&initial,&mapped_color));auto bytes=shader("cbuffer C:register(b0){float4 color;}float4 main():SV_Target{return color;}","ps_5_0");check(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&mapped_shader));auto* cb=mapped_color.Get();context->PSSetConstantBuffers(0,1,&cb);}
    ComPtr<ID3D11Texture1D> boxed_color,region_source;ComPtr<ID3D11ShaderResourceView> boxed_view;
    if(mode>=15 && mode<=20){D3D11_TEXTURE1D_DESC td{4,1,1,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};const float initial[16]{};D3D11_SUBRESOURCE_DATA data{initial,64,64};check(device->CreateTexture1D(&td,&data,&boxed_color));if(mode>=19)check(device->CreateTexture1D(&td,&data,&region_source));check(device->CreateShaderResourceView(boxed_color.Get(),nullptr,&boxed_view));auto* srv=boxed_view.Get();context->PSSetShaderResources(0,1,&srv);auto bytes=shader("Texture1D<float4> t;float4 main():SV_Target{return t.Load(int2(1,0));}","ps_5_0");check(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&mapped_shader));}
    ComPtr<ID3D11Texture2D> bc_source,bc_destination;ComPtr<ID3D11ShaderResourceView> bc_view;
    if(mode>=21 && mode<=22){const UINT blockSize=mode==21?8:16;std::array<std::array<uint8_t,64>,4> zero{};D3D11_SUBRESOURCE_DATA data[4]{};for(UINT i=0;i<4;++i)data[i]={zero[i].data(),blockSize*(i%2?1:2),0};D3D11_TEXTURE2D_DESC td{8,8,2,2,mode==21?DXGI_FORMAT_BC1_UNORM:DXGI_FORMAT_BC3_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};check(device->CreateTexture2D(&td,data,&bc_source));check(device->CreateTexture2D(&td,data,&bc_destination));check(device->CreateShaderResourceView(bc_destination.Get(),nullptr,&bc_view));auto* srv=bc_view.Get();context->PSSetShaderResources(0,1,&srv);auto bytes=shader("Texture2DArray<float4> t;float4 main():SV_Target{return t.Load(int4(0,0,1,1));}","ps_5_0");check(device->CreatePixelShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&mapped_shader));}
    ComPtr<ID3D11DeviceContext1> update_context;check(context.As(&update_context));
    ComPtr<ID3D11Query> frame_event;
    if(mode>=17 && mode<=22){
        if(mode==18){ComPtr<ID3D11Device3> device3;check(device.As(&device3));ComPtr<ID3D11Query1> event;D3D11_QUERY_DESC1 q{D3D11_QUERY_EVENT,0,D3D11_CONTEXT_TYPE_ALL};check(device3->CreateQuery1(&q,&event));check(event.As(&frame_event));}
        else{D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};check(device->CreateQuery(&q,&frame_event));}
    }
    ComPtr<IUnknown> initial_identity;check(buffer.As(&initial_identity));uint64_t identity_checks=0,unbind_checks=0;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp{0,0,8,8,0,1};context->RSSetViewports(1,&vp);
    std::array<ComPtr<ID3D11CommandList>,5> deferred_draws;
    if(mode==23){ComPtr<ID3D11DeviceContext> deferred;check(device->CreateDeferredContext(0,&deferred));for(size_t i=0;i<deferred_draws.size();++i){deferred->VSSetShader(vs.Get(),nullptr,0);deferred->PSSetShader(cycle[i].Get(),nullptr,0);deferred->OMSetRenderTargets(1,&view,nullptr);deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);deferred->RSSetViewports(1,&vp);deferred->Draw(3,0);check(deferred->FinishCommandList(FALSE,&deferred_draws[i]));}}
    std::unique_ptr<MsaaLiveRenderer> multisample;if(msaa)multisample=std::make_unique<MsaaLiveRenderer>(device.Get(),context.Get(),mode==24?2:4);
    ComPtr<IDXGISwapChain1> swap1;check(swap.As(&swap1));uint64_t frames=0;const auto deadline=GetTickCount64()+120000;
    while(!signal_exists(out/L"finish.signal")){
        if(GetTickCount64()>deadline)throw std::runtime_error("Live target control timeout");
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(signal_exists(out/L"pause.signal")){if(!signal_exists(out/L"paused.signal"))save(out/L"paused.signal",{{"frames",frames}});Sleep(5);continue;}
        if(signal_exists(out/L"paused.signal"))fs::remove(out/L"paused.signal");
        if(frame_event && frames){const auto until=GetTickCount64()+5000;for(;;){const auto hr=context->GetData(frame_event.Get(),nullptr,0,0);check(hr);if(hr==S_OK)break;if(GetTickCount64()>until)throw std::runtime_error("Target previous-frame EVENT timed out");Sleep(1);}}
        context->OMSetRenderTargets(1,&view,nullptr);context->PSSetShader(mode>=2?cycle[frames%5].Get():ps.Get(),nullptr,0);
        const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);
        if(mode>=11 && mode<=22){static const float values[5][4]{{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,1,1},{0,1,1,1}};if(mode>=21 && mode<=22){const uint16_t endpoints[]{0xf800,0x07e0,0x001f,0xffff,0x07ff};uint8_t bytes[16]{};if(mode==22)bytes[0]=255;memcpy(bytes+(mode==21?0:8),&endpoints[frames%5],2);context->UpdateSubresource(bc_source.Get(),1,nullptr,bytes,mode==21?8:16,0);D3D11_BOX box{0,0,0,4,4,1};if(mode==22)update_context->CopySubresourceRegion1(bc_destination.Get(),3,0,0,0,bc_source.Get(),1,&box,0);else context->CopySubresourceRegion(bc_destination.Get(),3,0,0,0,bc_source.Get(),1,&box);memset(bytes,0xcd,sizeof bytes);memset(&box,0,sizeof box);}
            else if(mode>=15){float color[4];memcpy(color,values[frames%5],16);D3D11_BOX box{1,0,0,2,1,1};if(mode==16 || mode==18)update_context->UpdateSubresource1(boxed_color.Get(),0,&box,color,16,16,0);else context->UpdateSubresource(mode>=19?region_source.Get():boxed_color.Get(),0,&box,color,16,16);if(mode>=19){if(mode==20)update_context->CopySubresourceRegion1(boxed_color.Get(),0,1,0,0,region_source.Get(),0,&box,0);else context->CopySubresourceRegion(boxed_color.Get(),0,1,0,0,region_source.Get(),0,&box);}memset(color,0xcd,sizeof color);memset(&box,0,sizeof box);}
            else if(mode>=13){float color[4];memcpy(color,values[frames%5],16);if(mode==14)update_context->UpdateSubresource1(mapped_color.Get(),0,nullptr,color,0,0,0);else context->UpdateSubresource(mapped_color.Get(),0,nullptr,color,0,0);memset(color,0xcd,sizeof color);}
            else{D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(mapped_color.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));memcpy(mapped.pData,values[frames%5],16);context->Unmap(mapped_color.Get(),0);}context->PSSetShader(mapped_shader.Get(),nullptr,0);}
        if(multisample)multisample->render(rtv.Get(),frames);else if(mode==23)context->ExecuteCommandList(deferred_draws[frames%5].Get(),TRUE);else context->Draw(3,0);
        if(signal_exists(out/L"update.signal")){UINT pixels[64]{};D3D11_BOX box{0,0,0,0,4,1};context->UpdateSubresource(buffer.Get(),0,&box,pixels,32,256);}
        if(frame_event)context->End(frame_event.Get());
        const UINT present_flags=tearing?DXGI_PRESENT_ALLOW_TEARING:0;
        HRESULT presented=S_OK;if(mode && mode!=24){DXGI_PRESENT_PARAMETERS parameters{};presented=swap1->Present1(0,present_flags,&parameters);}else presented=swap->Present(0,present_flags);
        check(presented);if(presented!=S_OK)throw std::runtime_error("Live oracle requires successful visible presentation");
        ComPtr<ID3D11Texture2D> current;check(swap->GetBuffer(0,IID_PPV_ARGS(&current)));ComPtr<IUnknown> identity;check(current.As(&identity));if(identity!=initial_identity)throw std::runtime_error("Logical backbuffer identity changed");++identity_checks;
        ComPtr<ID3D11RenderTargetView> bound;context->OMGetRenderTargets(1,&bound,nullptr);if(flip && bound)throw std::runtime_error("Flip Present did not unbind backbuffer");if(flip)++unbind_checks;
        ++frames;if(frames==3)save(out/L"ready.json",{{"frames",frames},{"mode",mode},{"window",reinterpret_cast<uintptr_t>(window)}});
        Sleep(10);
    }
    if(multisample){multisample->render(rtv.Get(),frames);multisample->verify(buffer.Get(),frames);}
    context->ClearState();context->Flush();save(out/L"exit.json",{{"frames",frames},{"ok",true},{"identity_checks",identity_checks},{"unbind_checks",unbind_checks},{"msaa_native_cpu_exact",bool(multisample)}});DestroyWindow(window);return 0;
}catch(const std::exception& e){save(out/L"error.json",{{"error",e.what()}});std::cerr<<e.what()<<'\n';return 1;}}
