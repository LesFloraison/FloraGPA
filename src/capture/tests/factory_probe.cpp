#include "protocol.hpp"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>

using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT result) { require(SUCCEEDED(result), "DXGI oracle call failed"); }
LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w,m,a,b); }
void save(const fs::path &path, const Json &value) {
    auto temp = path; temp += L".tmp";
    { std::ofstream out(temp); out << value.dump(2); if (!out) throw std::runtime_error("Write oracle"); }
    fs::rename(temp,path);
}
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) return 2;
    const fs::path out(argv[1]); fs::create_directories(out);
    try {
        const unsigned mode = std::stoul(argv[2]);
        ComPtr<IDXGIFactory2> factory;
        if (mode == 0) check(CreateDXGIFactory(IID_PPV_ARGS(&factory)));
        else if (mode == 1) check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        else check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        Json result; result["versions"] = Json::array();
        ComPtr<IUnknown> identity; check(factory.As(&identity));
        for (const auto &iid : {__uuidof(IUnknown),__uuidof(IDXGIObject),__uuidof(IDXGIFactory),__uuidof(IDXGIFactory1),
             __uuidof(IDXGIFactory2),__uuidof(IDXGIFactory3),__uuidof(IDXGIFactory4),__uuidof(IDXGIFactory5),__uuidof(IDXGIFactory6),__uuidof(IDXGIFactory7)}) {
            ComPtr<IUnknown> view; const auto hr = factory->QueryInterface(iid,reinterpret_cast<void **>(view.GetAddressOf()));
            bool same = false;
            if (SUCCEEDED(hr)) { ComPtr<IUnknown> back; check(view.As(&back)); same = back == identity; require(same,"Factory identity changed"); }
            result["versions"].push_back({{"result",uint32_t(hr)},{"identity",same}});
        }
        result["addref"] = factory->AddRef(); result["release"] = factory->Release();
        std::atomic<unsigned> failures{0}; std::vector<std::thread> threads;
        for (unsigned t=0;t<8;++t) threads.emplace_back([&] {
            for (unsigned i=0;i<1000;++i) {
                ComPtr<IDXGIFactory> view; ComPtr<IUnknown> back;
                if (FAILED(factory.As(&view)) || FAILED(view.As(&back)) || back != identity) ++failures;
            }
        });
        for(auto &t:threads)t.join(); require(failures==0,"Concurrent factory identity changed");
        result["concurrent_failures"] = failures.load();
        WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=proc; wc.lpszClassName=L"FloraFactoryOracle";
        RegisterClassW(&wc);
        HWND window=CreateWindowW(wc.lpszClassName,L"FloraGPA factory oracle",WS_OVERLAPPEDWINDOW,50,50,180,160,nullptr,nullptr,wc.hInstance,nullptr);
        require(window!=nullptr,"Create oracle window"); ShowWindow(window,SW_SHOWNOACTIVATE);
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<IDXGISwapChain> swap;
        if (mode==0) {
            DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferDesc.Width=64; desc.BufferDesc.Height=48; desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=1; desc.OutputWindow=window; desc.Windowed=TRUE;
            check(factory->CreateSwapChain(device.Get(),&desc,&swap));
        } else {
            DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=64; desc.Height=48; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            ComPtr<IDXGISwapChain1> view;
            if(mode==1) check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&view));
            else check(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,&view));
            check(view.As(&swap));
        }
        ComPtr<IUnknown> parent; check(swap->GetParent(IID_PPV_ARGS(&parent)));
        result["parent_identity"] = parent==identity;
        ComPtr<IUnknown> parentAgain; check(swap->GetParent(IID_PPV_ARGS(&parentAgain)));
        result["stable_parent_identity"] = parent == parentAgain;
        require(parent == parentAgain,"Repeated parent identity changed");
        ComPtr<ID3D11Texture2D> texture, staging; check(swap->GetBuffer(0,IID_PPV_ARGS(&texture)));
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        ComPtr<ID3D11RenderTargetView> rtv; check(device->CreateRenderTargetView(texture.Get(),nullptr,&rtv));
        const float color[]{0,1,0,1}; context->ClearRenderTargetView(rtv.Get(),color);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        check(device->CreateTexture2D(&desc,nullptr,&staging)); context->CopyResource(staging.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        unsigned pixelFailures=0;
        for(UINT y=0;y<desc.Height;++y) for(UINT x=0;x<desc.Width;++x) {
            auto p=static_cast<const uint8_t *>(mapped.pData)+y*mapped.RowPitch+x*4;
            if(p[0] || p[1]!=255 || p[2] || p[3]!=255)++pixelFailures;
        }
        context->Unmap(staging.Get(),0); result["pixel_failures"]=pixelFailures; require(!pixelFailures,"Factory pixels changed");
        result["test_present"] = uint32_t(swap->Present(0,DXGI_PRESENT_TEST));
        result["ok"]=true; save(out/L"ready.json",result);
        const auto deadline=GetTickCount64()+30000;
        while(!fs::exists(out/L"finish.signal")) {
            require(GetTickCount64()<deadline,"Factory oracle control timeout");
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);
        }
        return 0;
    } catch(const std::exception &e) { save(out/L"error.json",{{"error",e.what()}}); return 1; }
}

