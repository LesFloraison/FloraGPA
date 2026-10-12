#include "hud_pixels.hpp"
#include "protocol.hpp"
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
using namespace flora::capture::hud;
using Microsoft::WRL::ComPtr;
using flora::capture::Json;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
void bitmap(const std::filesystem::path& path,UINT width,UINT height,const std::vector<uint8_t>& rgba){
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+DWORD(rgba.size());
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(width);info.biHeight=-LONG(height);info.biPlanes=1;info.biBitCount=32;
    auto bgra=rgba;for(size_t i=0;i<bgra.size();i+=4)std::swap(bgra[i],bgra[i+2]);
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&file),sizeof file);out.write(reinterpret_cast<const char*>(&info),sizeof info);out.write(reinterpret_cast<const char*>(bgra.data()),bgra.size());
}
int main(int argc,char** argv){try{
    const std::filesystem::path out=argc==2?argv[1]:"";if(!out.empty())std::filesystem::create_directories(out);
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudLayoutOracle";require(RegisterClassW(&cls)!=0,"Window class failed");
    Json evidence=Json::array();unsigned index=0;
    for(const auto aware:{DPI_AWARENESS_CONTEXT_UNAWARE,DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2}){
        const auto original=SetThreadDpiAwarenessContext(aware);require(original!=nullptr,"Cannot select fixture DPI context");
        HWND window=CreateWindowW(cls.lpszClassName,L"Flora HUD layout oracle",WS_POPUP,40,40,800,600,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Cannot create fixture window");
        const auto dpi=GetDpiForWindow(window);require(dpi!=0,"Window DPI unavailable");
        DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=800;d.BufferDesc.Height=600;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.SampleDesc.Count=1;d.BufferCount=1;d.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;d.OutputWindow=window;d.Windowed=TRUE;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> swap;
        check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG|D3D11_CREATE_DEVICE_SINGLETHREADED,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&device,nullptr,&context));
        struct Size {UINT width,height,client_width,client_height;};
        for(const auto size:{Size{800,600,800,600},Size{400,300,800,600},Size{1600,900,800,600},Size{240,180,240,180},Size{800,100,800,100},Size{180,100,180,100},Size{800,600,800,600}}){
            require(SetWindowPos(window,nullptr,0,0,int(size.client_width),int(size.client_height),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"Cannot resize fixture client");
            check(swap->ResizeBuffers(1,size.width,size.height,DXGI_FORMAT_UNKNOWN,0));
            RECT client{};require(GetClientRect(window,&client)!=FALSE,"Client query failed");require(UINT(client.right)==size.client_width && UINT(client.bottom)==size.client_height,"Client did not resize");
            ComPtr<ID3D11Texture2D> buffer,stage;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC texture{};buffer->GetDesc(&texture);
            texture.BindFlags=0;texture.MiscFlags=0;texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&texture,nullptr,&stage));
            std::vector<uint32_t> expected(size_t(size.width)*size.height,0xb7603020u);context->UpdateSubresource(buffer.Get(),0,nullptr,expected.data(),size.width*4,0);
            const auto read=[&]{context->CopyResource(stage.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE m{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&m));std::vector<uint8_t> result(expected.size()*4);for(UINT row=0;row<size.height;++row)memcpy(result.data()+size_t(row)*size.width*4,static_cast<uint8_t*>(m.pData)+size_t(row)*m.RowPitch,size.width*4);context->Unmap(stage.Get(),0);return result;};
            const auto clean=read();
            for(const auto mode:{Mode::Compact,Mode::Detailed}){
                Panel panel;panel.mode=mode;panel.adapter=L"A deliberately very long adapter name that must stop before the red and green FPS values";panel.device=L"DX 11.0 [HW]";panel.mode_key=L"Ctrl+F1";panel.capture_key=L"Ctrl+Shift+C";panel.fps.current=60;panel.fps.minimum=48;panel.fps.maximum=72;panel.swap=42;
                const auto caller=aware==DPI_AWARENESS_CONTEXT_UNAWARE?DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2:DPI_AWARENESS_CONTEXT_UNAWARE;
                require(SetThreadDpiAwarenessContext(caller)!=nullptr,"Cannot select opposite caller DPI context");
                std::string reason;PixelTransaction hud;SetLastError(0x12345678);const bool rendered=hud.begin(swap.Get(),panel,0,false,reason);
                require(GetLastError()==0x12345678,"Layout changed LastError");require(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),caller),"HUD leaked window DPI context into application thread");
                const auto decorated=read();
                // Measure the actual changed pixels independently of panel_layout.
                UINT left=size.width,top=size.height,right=0,bottom=0;bool changed=false;
                for(UINT y=0;y<size.height;++y)for(UINT x=0;x<size.width;++x){const size_t at=(size_t(y)*size.width+x)*4;require(decorated[at+3]==clean[at+3],"HUD changed application alpha");if(memcmp(decorated.data()+at,clean.data()+at,4)){changed=true;left=(std::min)(left,x);top=(std::min)(top,y);right=(std::max)(right,x+1);bottom=(std::max)(bottom,y+1);}}
                require(changed==rendered,"Skipped layout changed pixels or rendered no pixels");
                if(rendered && size.client_width==800 && size.client_height==600){
                    const double density=double(dpi)/96;
                    require(std::abs(double(left)*800/size.width-12*density)<=double(800)/size.width,"Displayed horizontal margin changed with render resolution");
                    require(std::abs(double(top)*600/size.height-12*density)<=double(600)/size.height,"Displayed vertical margin changed with render resolution");
                    require(std::abs(double(right-left)*800/size.width-312*density)<=double(800)/size.width,"Displayed width changed with render resolution");
                    require(std::abs(double(bottom-top)*600/size.height-(mode==Mode::Compact?88:144)*density)<=double(600)/size.height,"Displayed height changed with render resolution");
                }
                if(size.client_width==180)require(!rendered,"Unreadable small window did not skip");
                require(hud.restore(reason),reason.c_str());require(read()==clean,"Layout/resize left HUD pixels in retained resource");
                if(!out.empty() && rendered)bitmap(out/(std::to_string(index)+".bmp"),size.width,size.height,decorated);
                evidence.push_back({{"index",index++},{"dpi",dpi},{"aware",aware==DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2},{"surface",{size.width,size.height}},{"client",{size.client_width,size.client_height}},{"mode",mode_name(mode)},{"rendered",rendered},{"reason",reason},{"changed_rectangle",{left,top,right,bottom}},{"exact_restore",true},{"caller_dpi_preserved",true}});
                SetThreadDpiAwarenessContext(aware);
            }
        }
        // DXGI_SCALING_NONE does not map the entire surface into the client.
        // Refuse that unequal-size path before touching pixels.
        swap.Reset();context->Flush();
        {
            ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory2> factory;
            check(device.As(&dxgi));check(dxgi->GetAdapter(&adapter));check(adapter->GetParent(IID_PPV_ARGS(&factory)));
            DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=400;desc.Height=300;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.Scaling=DXGI_SCALING_NONE;
            ComPtr<IDXGISwapChain1> no_stretch;check(factory->CreateSwapChainForHwnd(device.Get(),window,&desc,nullptr,nullptr,&no_stretch));
            ComPtr<ID3D11Texture2D> buffer,stage;check(no_stretch->GetBuffer(0,IID_PPV_ARGS(&buffer)));D3D11_TEXTURE2D_DESC texture{};buffer->GetDesc(&texture);texture.BindFlags=texture.MiscFlags=0;texture.Usage=D3D11_USAGE_STAGING;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateTexture2D(&texture,nullptr,&stage));
            std::vector<uint32_t> expected(400*300,0xb7603020u);context->UpdateSubresource(buffer.Get(),0,nullptr,expected.data(),1600,0);
            Panel panel;std::string reason;PixelTransaction hud;require(!hud.begin(no_stretch.Get(),panel,0,false,reason,true) && reason.find("stretch scaling")!=std::string::npos,"Non-stretch path was not rejected before drawing");
            context->CopyResource(stage.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE mapped{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&mapped));bool clean=true;for(UINT row=0;row<300;++row)clean&=memcmp(static_cast<const uint8_t*>(mapped.pData)+size_t(row)*mapped.RowPitch,expected.data()+size_t(row)*400,1600)==0;context->Unmap(stage.Get(),0);require(clean,"Non-stretch skip modified resource bytes");
            evidence.push_back({{"index",index++},{"dpi",dpi},{"mode","non-stretch"},{"rendered",false},{"reason",reason},{"exact_restore",true}});
        }
        ComPtr<ID3D11InfoQueue> info;check(device.As(&info));for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T length=0;check(info->GetMessage(i,nullptr,&length));std::vector<uint8_t> bytes(length);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,m,&length));if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(m->pDescription);}
        swap.Reset();DestroyWindow(window);SetThreadDpiAwarenessContext(original);
    }
    if(!out.empty()){std::ofstream file(out/"layout.json");file<<evidence.dump(2);}
    std::cout<<evidence.size()<<" native layout cases: resize, render resolution, DPI awareness, exact restoration, alpha and debug layer passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
