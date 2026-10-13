// Real native Presents to a hidden window. The window never activates, so
// this lock-order oracle neither needs nor claims on-screen presentation.
#include "hud_control.hpp"
#include "objects.hpp"
#include "swap_chain.hpp"
#include <d3d11sdklayers.h>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
Json run(HWND window,D3D_DRIVER_TYPE driver,DXGI_SWAP_EFFECT effect,bool extended,bool from_device,bool wrapped){
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=800;desc.BufferDesc.Height=600;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=effect;desc.Windowed=TRUE;desc.OutputWindow=window;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
    check(D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
    if(wrapped){wrap_object(device.GetAddressOf(),"Concurrent Present device");wrap_object(context.GetAddressOf(),"Concurrent Present context");wrap_swap_chain(swap.GetAddressOf());}
    ComPtr<IDXGISwapChain1> swap1;check(swap.As(&swap1));
    ComPtr<ID3D11Multithread> protection;check(from_device?device.As(&protection):context.As(&protection));protection->SetMultithreadProtected(TRUE);
    ComPtr<ID3D11Texture2D> buffer,stage;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view));
    const float color[]{32.f/255,48.f/255,96.f/255,1};context->ClearRenderTargetView(view.Get(),color);
    Json warmup=Json::array();
    for(UINT slot=0;slot<2;++slot){context->ClearRenderTargetView(view.Get(),color);const auto hr=swap->Present(0,0);require(hr==S_OK || hr==DXGI_STATUS_OCCLUDED,"Native warmup failed");warmup.push_back(uint32_t(hr));}
    D3D11_TEXTURE2D_DESC td{};buffer->GetDesc(&td);td.BindFlags=td.MiscFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&td,nullptr,&stage));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;ComPtr<ID3D11Buffer> marker,copy;check(device->CreateBuffer(&bd,nullptr,&marker));bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateBuffer(&bd,nullptr,&copy));
    Json calls=Json::array();
    for(UINT iteration=0;iteration<8;++iteration){
        protection->Enter();protection->Enter();std::atomic<bool> started=false,done=false;HRESULT result=E_FAIL;DWORD last=0;std::exception_ptr failure;
        std::jthread presenter([&]{try{started=true;SetLastError(0x51a7);DXGI_PRESENT_PARAMETERS parameters{};result=extended?swap1->Present1(0,0,&parameters):swap->Present(0,0);last=GetLastError();}catch(...){failure=std::current_exception();}done=true;});
        while(!started)std::this_thread::yield();Sleep(10);
        // An application holding native Enter may keep using its immediate
        // context while another thread attempts Present. No alias/capture gate
        // may be held by that waiter while it waits for native protection.
        const bool owner_type=context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;
        const std::array<UINT,4> expected{iteration,iteration+17,iteration+31,0x12345678};
        context->UpdateSubresource(marker.Get(),0,nullptr,expected.data(),0,0);
        context->CopyResource(copy.Get(),marker.Get());context->Flush();
        protection->Leave();const bool nested_type=context->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE;protection->Leave();
        // Native Present can wait for window messages; keep the owner's pump
        // available while observing completion, without activating the window.
        while(!done){pump();Sleep(1);}presenter.join();if(failure)std::rethrow_exception(failure);
        require(owner_type && nested_type,"Lock owner context access failed");
        require(result==S_OK || result==DXGI_STATUS_OCCLUDED,"Native concurrent Present failed");
        if(wrapped){const auto status=hud_status();require(status["swap_chains"].size()==1,"HUD did not observe native Present");require(status["swap_chains"][0]["reason"]=="","HUD skipped instead of exercising concurrent presentation");if(result==DXGI_STATUS_OCCLUDED)require(!status["visible"].get<bool>(),"Occluded HUD reported visible");}
        D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped));const bool exact_marker=memcmp(mapped.pData,expected.data(),sizeof expected)==0;context->Unmap(copy.Get(),0);require(exact_marker,"Owner GPU update/copy changed across concurrent Present");
        context->CopyResource(stage.Get(),buffer.Get());check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped));bool exact_scene=true;
        for(UINT y=0;y<600;++y){const auto* row=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch);for(UINT x=0;x<800;++x)exact_scene&=row[x]==0xff603020u;}context->Unmap(stage.Get(),0);require(exact_scene,"Concurrent HUD Present contaminated retained pixels");
        calls.push_back({{"result",uint32_t(result)},{"last_error",last},{"marker_exact",exact_marker},{"scene_exact",exact_scene}});
    }
    Json warnings=Json::array();for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)warnings.push_back({{"id",m->ID},{"text",m->pDescription}});}
    require(warnings.empty(),warnings.dump().c_str());context->ClearState();context->Flush();
    return {{"wrapped",wrapped},{"effect",effect},{"present1",extended},{"device_origin",from_device},{"warmup",warmup},{"calls",calls},{"debug_warnings",warnings}};
}
int main(int argc,char**){try{
    std::jthread watchdog([](std::stop_token stop){for(unsigned i=0;i<4500 && !stop.stop_requested();++i)Sleep(10);if(!stop.stop_requested()){std::cerr<<"Native Present/context lock-order timeout\n"<<std::flush;ExitProcess(86);}});
    hud::Settings settings;settings.mode_key={};settings.capture_key={};configure_hud(settings);
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraConcurrentPresent";require(RegisterClassW(&cls)!=0,"Window class failed");Json rows=Json::array();
    const auto foreground=GetForegroundWindow();
    for(const auto effect:{DXGI_SWAP_EFFECT_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL})for(bool extended:{false,true})for(bool from_device:{false,true})for(bool wrapped:{false,true}){
        HWND window=CreateWindowExW(WS_EX_NOACTIVATE,cls.lpszClassName,L"Flora concurrent Present oracle",WS_OVERLAPPEDWINDOW,100,100,840,660,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Window failed");
        rows.push_back(run(window,argc>1?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,effect,extended,from_device,wrapped));DestroyWindow(window);pump();
        require(object_status()["active"]==0 && metadata_status()["alive"]==0,"Concurrent Present objects survived teardown");require(object_status().value("interface_gaps",0ull)==0,"Concurrent Present interface gap");
    }
    disconnect_hud();watchdog.request_stop();std::cout<<Json{{"passed",true},{"driver",argc>1?"hardware":"WARP"},{"native_wrapped_pairs",8},{"concurrent_present_calls",128},{"warmup_presents",32},{"foreground_unchanged",GetForegroundWindow()==foreground},{"scope","Real native Present/Present1 to hidden windows; no on-screen visibility claim"},{"rows",rows}}.dump(2)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
