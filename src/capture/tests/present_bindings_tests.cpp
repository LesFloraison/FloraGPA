#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <vector>
#include "present_bindings.hpp"
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int main(int argc,char**){try{
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraPresentBindings";RegisterClassW(&cls);
    HWND window=CreateWindowW(cls.lpszClassName,L"Flora Present bindings",WS_OVERLAPPEDWINDOW,100,100,160,160,nullptr,nullptr,cls.hInstance,nullptr);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);
    for(unsigned i=0;i<20;++i){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}
    Json native_rows=Json::array(),facade_rows=Json::array();
    for(bool facade:{false,true})for(auto level:{D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_11_1})for(bool extended:{false,true})for(auto effect:{DXGI_SWAP_EFFECT_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,DXGI_SWAP_EFFECT_FLIP_DISCARD}){
        DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=d.BufferDesc.Height=64;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_SHADER_INPUT|DXGI_USAGE_UNORDERED_ACCESS;d.SampleDesc.Count=1;d.BufferCount=2;d.SwapEffect=effect;d.OutputWindow=window;d.Windowed=TRUE;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,&level,1,D3D11_SDK_VERSION,&d,&swap,&device,nullptr,&context));ComPtr<IDXGISwapChain1> swap1;check(swap.As(&swap1));
        ComPtr<ID3D11Texture2D> buffer,other;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC td{};buffer->GetDesc(&td);td.MiscFlags=0;check(device->CreateTexture2D(&td,nullptr,&other));
        ComPtr<ID3D11Texture2D> real=buffer;if(facade){buffer.Reset();check(device->CreateTexture2D(&td,nullptr,&buffer));}
        ComPtr<ID3D11RenderTargetView> rtv,other_rtv;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));check(device->CreateRenderTargetView(other.Get(),nullptr,&other_rtv));
        ComPtr<ID3D11ShaderResourceView> srv,other_srv;check(device->CreateShaderResourceView(buffer.Get(),nullptr,&srv));check(device->CreateShaderResourceView(other.Get(),nullptr,&other_srv));
        ComPtr<ID3D11UnorderedAccessView> uav,other_uav;check(device->CreateUnorderedAccessView(buffer.Get(),nullptr,&uav));
        D3D11_BUFFER_DESC counter_desc{};counter_desc.ByteWidth=64;counter_desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;counter_desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;counter_desc.StructureByteStride=4;ComPtr<ID3D11Buffer> counter;check(device->CreateBuffer(&counter_desc,nullptr,&counter));D3D11_UNORDERED_ACCESS_VIEW_DESC counter_view{};counter_view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;counter_view.Buffer.NumElements=16;counter_view.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;check(device->CreateUnorderedAccessView(counter.Get(),&counter_view,&other_uav));
        D3D11_BUFFER_DESC read_desc{};read_desc.ByteWidth=4;ComPtr<ID3D11Buffer> counter_copy,counter_stage;check(device->CreateBuffer(&read_desc,nullptr,&counter_copy));read_desc.Usage=D3D11_USAGE_STAGING;read_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateBuffer(&read_desc,nullptr,&counter_stage));
        ComPtr<ID3D11InfoQueue> info;check(device.As(&info));
        for(UINT variant=0;variant<4;++variant)for(UINT mode=0;mode<4;++mode){
            const UINT flags=variant==1?DXGI_PRESENT_TEST:variant==2?DXGI_PRESENT_DO_NOT_SEQUENCE:0;
            const UINT interval=variant==3?5:1;
            context->ClearState();
            if(mode==0){ID3D11RenderTargetView* views[]{rtv.Get(),other_rtv.Get()};context->OMSetRenderTargets(2,views,nullptr);}
            if(mode==1){ID3D11ShaderResourceView* views[]{srv.Get(),other_srv.Get()};context->VSSetShaderResources(3,2,views);context->HSSetShaderResources(3,2,views);context->DSSetShaderResources(3,2,views);context->GSSetShaderResources(3,2,views);context->PSSetShaderResources(3,2,views);context->CSSetShaderResources(3,2,views);}
            const UINT initial[]{UINT(-1),37};
            if(mode==2){ID3D11UnorderedAccessView* views[]{uav.Get(),other_uav.Get()};context->CSSetUnorderedAccessViews(3,2,views,initial);}
            if(mode==3){ID3D11UnorderedAccessView* views[]{uav.Get(),other_uav.Get()};context->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,3,2,views,initial);}
            std::unique_ptr<flora::capture::hud::PresentBindings> bindings;
            if(facade && !(flags&DXGI_PRESENT_TEST)){SetLastError(0x8765);bindings=std::make_unique<flora::capture::hud::PresentBindings>(context.Get(),real.Get(),buffer.Get());if(GetLastError()!=0x8765)throw std::runtime_error("Binding preparation changed LastError");}
            DXGI_PRESENT_PARAMETERS parameters{};const auto hr=extended?swap1->Present1(interval,flags,&parameters):swap->Present(interval,flags);if(bindings){SetLastError(0x9876);bindings.reset();if(GetLastError()!=0x9876)throw std::runtime_error("Binding restoration changed LastError");}Json row={{"effect",effect},{"feature_level",level},{"extended",extended},{"flags",flags},{"interval",interval},{"mode",mode},{"hr",uint32_t(hr)}};
            if((variant==3 && SUCCEEDED(hr)) || (variant!=3 && hr!=S_OK)){RECT client{},position{};GetClientRect(window,&client);GetWindowRect(window,&position);std::cerr<<Json{{"hr",uint32_t(hr)},{"effect",effect},{"mode",mode},{"facade",facade},{"variant",variant},{"foreground",GetForegroundWindow()==window},{"visible",bool(IsWindowVisible(window))},{"iconic",bool(IsIconic(window))},{"client",{client.left,client.top,client.right,client.bottom}},{"position",{position.left,position.top,position.right,position.bottom}},{"original_shim_loaded",GetModuleHandleW(L"shimd3d64.dll")!=nullptr}}.dump()<<'\n';if(argc>1){const auto until=GetTickCount64()+45000;while(GetTickCount64()<until){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(20);}}throw std::runtime_error("Unexpected native Present result");}
            ComPtr<ID3D11RenderTargetView> a,b;ID3D11RenderTargetView* rt[2]{};context->OMGetRenderTargets(2,rt,nullptr);a.Attach(rt[0]);b.Attach(rt[1]);if((a && a!=rtv) || (b && b!=other_rtv))throw std::runtime_error("RTV mirror leaked through getter");row["rtv"]={bool(a),bool(b)};
            Json stages=Json::array();
            using Getter=void(STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT,UINT,ID3D11ShaderResourceView**);
            for(Getter getter:{&ID3D11DeviceContext::VSGetShaderResources,&ID3D11DeviceContext::HSGetShaderResources,&ID3D11DeviceContext::DSGetShaderResources,&ID3D11DeviceContext::GSGetShaderResources,&ID3D11DeviceContext::PSGetShaderResources,&ID3D11DeviceContext::CSGetShaderResources}){ID3D11ShaderResourceView* views[2]{};(context.Get()->*getter)(3,2,views);if((views[0] && views[0]!=srv.Get()) || (views[1] && views[1]!=other_srv.Get()))throw std::runtime_error("SRV mirror leaked through getter");stages.push_back({views[0]!=nullptr,views[1]!=nullptr});for(auto* v:views)if(v)v->Release();}row["srv"]=stages;
            ID3D11UnorderedAccessView* views[2]{};context->CSGetUnorderedAccessViews(3,2,views);if((views[0] && views[0]!=uav.Get()) || (views[1] && views[1]!=other_uav.Get()))throw std::runtime_error("CS UAV mirror leaked through getter");row["cs_uav"]={views[0]!=nullptr,views[1]!=nullptr};for(auto* v:views)if(v)v->Release();context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,3,2,views);if((views[0] && views[0]!=uav.Get()) || (views[1] && views[1]!=other_uav.Get()))throw std::runtime_error("OM UAV mirror leaked through getter");row["om_uav"]={views[0]!=nullptr,views[1]!=nullptr};for(auto* v:views)if(v)v->Release();
            if(mode>=2){context->CopyStructureCount(counter_copy.Get(),0,other_uav.Get());context->CopyResource(counter_stage.Get(),counter_copy.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(counter_stage.Get(),0,D3D11_MAP_READ,0,&mapped));const auto count=*static_cast<const UINT*>(mapped.pData);context->Unmap(counter_stage.Get(),0);if(count!=37)throw std::runtime_error("Unrelated UAV counter changed");row["unrelated_uav_counter"]=count;}
            row["diagnostics"]=Json::array();for(UINT64 n=0;n<info->GetNumStoredMessages();++n){SIZE_T size=0;check(info->GetMessage(n,nullptr,&size));std::vector<uint8_t> storage(size);auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());check(info->GetMessage(n,message,&size));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)row["diagnostics"].push_back({{"id",message->ID},{"severity",message->Severity}});}info->ClearStoredMessages();
            (facade?facade_rows:native_rows).push_back(row);
        }
        context->ClearState();context->Flush();
    }
    DestroyWindow(window);const bool equal=native_rows==facade_rows;std::cout<<Json{{"passed",equal},{"native",native_rows},{"facade",facade_rows}}.dump(2)<<'\n';return equal?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
