// Native oracle application. Uses only Windows/D3D APIs; no capture exports.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <thread>
#include <atomic>
using Microsoft::WRL::ComPtr;
using Json=nlohmann::json;
namespace fs=std::filesystem;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void hr(HRESULT value){require(SUCCEEDED(value),"Unexpected native HRESULT");}
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
static void save(const fs::path& path,const Json& value){
    auto temp=path;temp+=L".tmp";std::ofstream file(temp);file<<value.dump(2);file.close();
    require(bool(file),"Write oracle");fs::rename(temp,path);
}
static void wait(const fs::path& path){
    const auto deadline=GetTickCount64()+20000;
    while(!fs::exists(path)){
        require(GetTickCount64()<deadline,"Oracle control timeout");
        MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(10);
    }
}
static Json image(ID3D11Device* device,ID3D11DeviceContext* context,IDXGISwapChain* swap){
    ComPtr<ID3D11Texture2D> texture,staging;hr(swap->GetBuffer(0,IID_PPV_ARGS(&texture)));
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    ComPtr<ID3D11RenderTargetView> view;hr(device->CreateRenderTargetView(texture.Get(),nullptr,&view));
    const float color[]={1,0,0,1};context->ClearRenderTargetView(view.Get(),color);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    hr(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{};hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
    unsigned failures=0;
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){
        auto pixel=static_cast<const unsigned char*>(map.pData)+y*map.RowPitch+x*4;
        if(pixel[0]!=255 || pixel[1] || pixel[2] || pixel[3]!=255)++failures;
    }
    context->Unmap(staging.Get(),0);return { {"width",desc.Width},{"height",desc.Height},{"pixel_failures",failures} };
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;const fs::path out=argv[1];fs::create_directories(out);
    try{
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=proc;wc.lpszClassName=L"FloraCaptureSwapOracle";
        RegisterClassW(&wc);HWND window=CreateWindowW(wc.lpszClassName,L"FloraGPA swap-chain oracle",WS_OVERLAPPEDWINDOW,
            50,50,180,160,nullptr,nullptr,wc.hInstance,nullptr);require(window!=nullptr,"Create window");ShowWindow(window,SW_SHOWNOACTIVATE);
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=64;desc.BufferDesc.Height=64;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        auto create=[&](){hr(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &desc,&swap,&device,nullptr,&context));};
        create();Json result;result["versions"]=Json::array();
        {
            ComPtr<IUnknown> identity;hr(swap.As(&identity));
            for(const GUID& iid:{__uuidof(IUnknown),__uuidof(IDXGIObject),__uuidof(IDXGIDeviceSubObject),__uuidof(IDXGISwapChain),
                                 __uuidof(IDXGISwapChain1),__uuidof(IDXGISwapChain2),__uuidof(IDXGISwapChain3),__uuidof(IDXGISwapChain4),__uuidof(IDXGISwapChainMedia)}){
                ComPtr<IUnknown> view;const auto code=swap->QueryInterface(iid,reinterpret_cast<void**>(view.GetAddressOf()));
                bool same=false;if(SUCCEEDED(code)){ComPtr<IUnknown> back;hr(view.As(&back));same=back.Get()==identity.Get();require(same,"Broken IUnknown identity");}
                result["versions"].push_back({{"result",static_cast<std::uint32_t>(code)},{"identity",same}});
            }
            const GUID missing={0xd080caf1,0x5513,0x426b,{0xbe,0xd0,0x1b,0x3e,0xe9,0x15,0xac,0x63}};
            void* unavailable=reinterpret_cast<void*>(1);auto code=swap->QueryInterface(missing,&unavailable);
            result["unknown_query"]={static_cast<std::uint32_t>(code),unavailable==nullptr};require(code==E_NOINTERFACE && !unavailable,"Unknown query changed");
            result["null_query"]=static_cast<std::uint32_t>(swap->QueryInterface(__uuidof(IDXGISwapChain),nullptr));
            result["addref"]=swap->AddRef();result["release"]=swap->Release();
            ComPtr<ID3D11Device> from_swap;hr(swap->GetDevice(IID_PPV_ARGS(&from_swap)));
            result["device_identity"]=from_swap.Get()==device.Get();require(result["device_identity"].get<bool>(),"Device identity changed");
            const GUID data_key={0xd080caf2,0x5513,0x426b,{0xbe,0xd0,0x1b,0x3e,0xe9,0x15,0xac,0x63}};
            const UINT value=0xa123f005;hr(swap->SetPrivateData(data_key,sizeof(value),&value));UINT copy=0,size=sizeof(copy);
            hr(swap->GetPrivateData(data_key,&size,&copy));result["private_data"]=copy;require(copy==value,"Private data changed");
            hr(swap->SetPrivateDataInterface(data_key,swap.Get()));
            ComPtr<IUnknown> stored;size=sizeof(IUnknown*);hr(swap->GetPrivateData(data_key,&size,stored.GetAddressOf()));
            ComPtr<IUnknown> stored_identity;hr(stored.As(&stored_identity));
            result["private_interface_identity"]=stored_identity.Get()==identity.Get();
            require(result["private_interface_identity"].get<bool>(),"Private IUnknown identity changed");
            stored_identity.Reset();stored.Reset();hr(swap->SetPrivateDataInterface(data_key,nullptr));
            std::atomic<unsigned> failures{0};std::vector<std::thread> workers;
            for(unsigned t=0;t<8;++t)workers.emplace_back([&](){
                for(unsigned i=0;i<1000;++i){
                    ComPtr<IDXGISwapChain1> view;ComPtr<IUnknown> canonical;
                    if(FAILED(swap.As(&view)) || FAILED(view.As(&canonical)) || canonical.Get()!=identity.Get())++failures;
                }
            });
            for(auto& worker:workers)worker.join();result["concurrent_identity_failures"]=failures.load();
            require(failures==0,"Concurrent query identity changed");
            result["post_concurrency_addref"]=swap->AddRef();result["post_concurrency_release"]=swap->Release();
        }
        result["first_image"]=image(device.Get(),context.Get(),swap.Get());
        result["presents"]=Json::array();
        for(UINT flags:{UINT(DXGI_PRESENT_TEST),0u,0u,0u}){
            SetLastError(0xc0de1234);const HRESULT present=swap->Present(0,flags);const DWORD error=GetLastError();
            result["presents"].push_back({{"flags",flags},{"result",static_cast<std::uint32_t>(present)},{"last_error",error}});
        }
        {
            ComPtr<ID3D11Texture2D> held;hr(swap->GetBuffer(0,IID_PPV_ARGS(&held)));
            result["held_resize"]=static_cast<std::uint32_t>(swap->ResizeBuffers(0,80,48,DXGI_FORMAT_UNKNOWN,0));
            require(result["held_resize"].get<std::uint32_t>()==static_cast<std::uint32_t>(DXGI_ERROR_INVALID_CALL),"Held buffer unexpectedly resized");
            DXGI_SWAP_CHAIN_DESC current{};hr(swap->GetDesc(&current));
            require(current.BufferDesc.Width==64 && current.BufferDesc.Height==64,"Failed resize changed description");
            save(out/L"held-failure.json",{{"width",current.BufferDesc.Width},{"height",current.BufferDesc.Height}});
            wait(out/L"resize.signal");
        }
        context->ClearState();context->Flush();auto resized=swap->ResizeBuffers(0,80,48,DXGI_FORMAT_UNKNOWN,0);hr(resized);
        result["resize"]=static_cast<std::uint32_t>(resized);
        {
            ComPtr<IDXGISwapChain1> version;auto code=swap.As(&version);result["version1"]=static_cast<std::uint32_t>(code);
            if(SUCCEEDED(code)){
                DXGI_SWAP_CHAIN_DESC1 current{};hr(version->GetDesc1(&current));HWND actual=nullptr;hr(version->GetHwnd(&actual));
                require(actual==window && current.Width==80 && current.Height==48,"Versioned getters changed");
                DXGI_PRESENT_PARAMETERS parameters{};
                result["present1_test"]=static_cast<std::uint32_t>(version->Present1(0,DXGI_PRESENT_TEST,&parameters));
            }
        }
        result["resized_image"]=image(device.Get(),context.Get(),swap.Get());
        save(out/L"ready.json",result);wait(out/L"release.signal");
        context->ClearState();context->Flush();swap.Reset();context.Reset();device.Reset();
        save(out/L"released.json",{{"released",true}});wait(out/L"recreate.signal");
        desc.BufferDesc.Width=32;desc.BufferDesc.Height=32;create();
        auto second=image(device.Get(),context.Get(),swap.Get());save(out/L"second.json",second);wait(out/L"finish.signal");
        context->ClearState();swap.Reset();context.Reset();device.Reset();DestroyWindow(window);
        save(out/L"done.json",{{"ok",true}});return 0;
    }catch(const std::exception& error){save(out/L"error.json",{{"error",error.what()}});return 1;}
}
