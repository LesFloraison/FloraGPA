// Native development experiment for clean application buffers plus a decorated
// presentation surface. This does not enable the runtime's multi-buffer HUD.
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include "clean_buffer_ring.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
namespace {
constexpr UINT extent=32;
using Pixels=std::array<uint32_t,extent*extent>;
void check(HRESULT value){if(FAILED(value))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(value)));}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
LRESULT CALLBACK procedure(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
struct Window {
    HWND value;
    Window(){value=CreateWindowW(L"FloraCleanBufferProbe",L"Flora clean buffer experiment",WS_OVERLAPPEDWINDOW,80,80,160,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);require(value!=nullptr,"Create window failed");ShowWindow(value,SW_SHOW);SetForegroundWindow(value);for(unsigned i=0;i<20;++i){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}}
    ~Window(){DestroyWindow(value);}
};
struct Run {std::vector<Pixels> before,after;UINT decorated=0,debug_errors=0;};
Run run(DXGI_SWAP_EFFECT effect,UINT count,bool isolated,bool tearing,bool nonsequencing,DXGI_FORMAT format){
    Window window;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<IDXGIDevice> dxgi;check(device.As(&dxgi));ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(&adapter));ComPtr<IDXGIFactory2> factory;check(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=desc.Height=extent;desc.Format=format;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=count;desc.SwapEffect=effect;desc.Flags=tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1> swap;check(factory->CreateSwapChainForHwnd(device.Get(),window.value,&desc,nullptr,nullptr,&swap));
    ComPtr<ID3D11Texture2D> real;check(swap->GetBuffer(0,IID_PPV_ARGS(&real)));
    D3D11_TEXTURE2D_DESC texture{};real->GetDesc(&texture);texture.MiscFlags=0;texture.BindFlags=0;
    flora::capture::hud::CleanBufferRing clean(swap.Get());ComPtr<ID3D11Texture2D> stage;
    texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&texture,nullptr,&stage));
    const auto read=[&](ID3D11Texture2D* source){context->CopyResource(stage.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped));Pixels pixels{};for(UINT y=0;y<extent;++y)memcpy(pixels.data()+y*extent,static_cast<uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,extent*4);context->Unmap(stage.Get(),0);return pixels;};
    Pixels base{};base.fill(0xff603020u);for(UINT slot=0;slot<clean.size();++slot)context->UpdateSubresource(clean.buffer(slot),0,nullptr,base.data(),extent*4,0);
    ComPtr<ID3D11Predicate> predicate;D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE,0};check(device->CreatePredicate(&query,&predicate));context->Begin(predicate.Get());context->End(predicate.Get());BOOL observed=TRUE;const auto deadline=GetTickCount64()+5000;HRESULT query_result=S_FALSE;while((query_result=context->GetData(predicate.Get(),&observed,sizeof observed,0))==S_FALSE && GetTickCount64()<deadline)Sleep(1);require(query_result==S_OK && !observed,"Empty predicate did not resolve false");
    const auto predicated_private_copy=[&](UINT frame,auto&& operation){
        const BOOL value=BOOL(frame%2);context->SetPredication(predicate.Get(),value);operation();
        ComPtr<ID3D11Predicate> actual;BOOL actual_value=FALSE;context->GetPredication(&actual,&actual_value);require(actual==predicate && actual_value==value,"Private copies changed application predication");context->SetPredication(nullptr,FALSE);
    };
    Run result;const UINT flags=(tearing?DXGI_PRESENT_ALLOW_TEARING:0)|(nonsequencing?DXGI_PRESENT_DO_NOT_SEQUENCE:0);
    // Warm up all native slots; no assumption about initial undefined contents.
    for(UINT i=0;i<count;++i){context->UpdateSubresource(real.Get(),0,nullptr,base.data(),extent*4,0);require(swap->Present(tearing?0:1,flags&~DXGI_PRESENT_DO_NOT_SEQUENCE)==S_OK,"Warmup Present did not succeed");}
    for(UINT frame=0;frame<24;++frame){
        auto* application=isolated?clean.buffer(0):real.Get();
        if(effect==DXGI_SWAP_EFFECT_FLIP_DISCARD)context->UpdateSubresource(application,0,nullptr,base.data(),extent*4,0);
        // Leave the HUD region untouched. Partial changes accumulate separately
        // in each retained physical buffer, exposing an incorrect rotation.
        const UINT x=8+frame%16,y=8+(frame/16);const uint32_t marker=0xff000000u|((frame+1)*997u);
        D3D11_BOX point{x,y,0,x+1,y+1,1};context->UpdateSubresource(application,0,&point,&marker,4,0);
        result.before.push_back(read(application));
        const auto before_test=read(application);check(swap->Present(0,DXGI_PRESENT_TEST));require(read(application)==before_test,"TEST changed application pixels");
        if(isolated){
            std::vector<Pixels> unchanged;for(UINT slot=0;slot<count;++slot)unchanged.push_back(read(clean.buffer(slot)));
            SetLastError(0x4312);clean.prepare(swap.Get(),DXGI_PRESENT_TEST);clean.advance(S_OK,DXGI_PRESENT_TEST);
            clean.advance(E_FAIL,0);clean.advance(DXGI_STATUS_OCCLUDED,0);
            require(GetLastError()==0x4312,"Clean ring no-op changed LastError");
            for(UINT slot=0;slot<count;++slot)require(read(clean.buffer(slot))==unchanged[slot],"Non-rotating result changed clean buffers");
            predicated_private_copy(frame,[&]{clean.prepare(swap.Get(),flags);});
            require(read(real.Get())==read(application),"Predicate suppressed private presentation copy");
            const uint32_t green=0xff00ff00u;D3D11_BOX hud{0,0,0,1,1,1};context->UpdateSubresource(real.Get(),0,&hud,&green,4,0);
            require(read(real.Get())[0]==green,"Decorated presentation surface missing HUD marker");
            require(read(application)[0]==base[0],"HUD changed application surface before Present");++result.decorated;
        }
        DXGI_PRESENT_PARAMETERS parameters{};
        const auto hr=frame%2?swap->Present1(tearing?0:1,flags,&parameters):swap->Present(tearing?0:1,flags);
        require(hr==S_OK,"Present did not succeed");
        if(isolated){
            predicated_private_copy(frame,[&]{clean.advance(hr,flags);});
        }
        if(effect!=DXGI_SWAP_EFFECT_FLIP_DISCARD){
            for(UINT slot=0;slot<count;++slot){
                ComPtr<ID3D11Texture2D> view;if(!isolated)check(swap->GetBuffer(slot,IID_PPV_ARGS(&view)));
                result.after.push_back(read(isolated?clean.buffer(slot):view.Get()));
            }
        }
    }
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
    for(UINT64 n=0;n<info->GetNumStoredMessages();++n){SIZE_T bytes=0;check(info->GetMessage(n,nullptr,&bytes));std::vector<uint8_t> storage(bytes);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(info->GetMessage(n,message,&bytes));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){++result.debug_errors;std::cerr<<message->pDescription<<'\n';}}
    context->ClearState();context->Flush();return result;
}
}
int main(){try{
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraCleanBufferProbe";require(RegisterClassW(&cls)!=0,"Register class failed");
    Json rows=Json::array();
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM})for(auto effect:{DXGI_SWAP_EFFECT_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_DISCARD})for(UINT count:{2u,3u})for(bool tearing:{false,true})for(bool nonsequencing:{false,true}){
        if((tearing && effect==DXGI_SWAP_EFFECT_SEQUENTIAL) || (tearing && nonsequencing))continue;
        const auto native=run(effect,count,false,tearing,nonsequencing,format);const auto isolated=run(effect,count,true,tearing,nonsequencing,format);
        if(native.before!=isolated.before || native.after!=isolated.after)std::cerr<<Json{{"format",format},{"effect",effect},{"buffers",count},{"tearing",tearing},{"nonsequencing",nonsequencing}}.dump()<<'\n';
        require(native.before==isolated.before,"Clean buffer pre-Present contents differ from native");
        require(native.after==isolated.after,"Clean buffer rotation differs from native");
        require(!native.debug_errors && !isolated.debug_errors,"Debug layer diagnostics");
        rows.push_back({{"format",format},{"effect",effect},{"buffers",count},{"tearing",tearing},{"nonsequencing",nonsequencing},{"frames",native.before.size()},{"decorated_frames",isolated.decorated},{"readable_slot_checks",native.after.size()},{"exact",true}});
    }
    std::cout<<Json{{"passed",true},{"scope","Native clean-buffer algorithm experiment, not runtime COM virtualization or capture acceptance"},{"profiles",rows}}.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
