#include "capture_session.hpp"
#include "swap_chain.hpp"
#include "objects.hpp"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <future>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void check(HRESULT hr){require(SUCCEEDED(hr),"Present oracle HRESULT failed");}
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
struct Fixture {
    HWND window=nullptr;uint64_t id=0;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;ComPtr<ID3D11RenderTargetView> rtv;
    Fixture(){
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraPresentCaptureOracle";RegisterClassW(&cls);
        window=CreateWindowW(cls.lpszClassName,L"Flora capture boundary test",WS_OVERLAPPEDWINDOW,0,0,160,160,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Cannot create capture test window");ShowWindow(window,SW_SHOWNOACTIVATE);
        DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=d.BufferDesc.Height=8;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=1;d.OutputWindow=window;d.Windowed=TRUE;d.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&swap,&device,nullptr,&context));
        wrap_swap_chain(swap.GetAddressOf());wrap_object(device.GetAddressOf(),"PresentTest::Device");wrap_object(context.GetAddressOf(),"PresentTest::Context");
        const auto observed=swap_chain_status();for(const auto& row:observed["active"])if(row["window"]==reinterpret_cast<uintptr_t>(window))id=row["id"].get<uint64_t>();require(id!=0,"Missing observed swap identity");
        target();
        const auto shader=[&](const char* text,const char* profile){ComPtr<ID3DBlob> blob;check(D3DCompile(text,strlen(text),"present-oracle",nullptr,nullptr,"main",profile,0,0,&blob,nullptr));return blob;};
        auto code=shader("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}","vs_5_0");ComPtr<ID3D11VertexShader> vs;check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));context->VSSetShader(vs.Get(),nullptr,0);
        code=shader("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0");ComPtr<ID3D11PixelShader> ps;check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));context->PSSetShader(ps.Get(),nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT viewport{0,0,8,8,0,1};context->RSSetViewports(1,&viewport);
    }
    void target(){ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));check(device->CreateRenderTargetView(buffer.Get(),nullptr,&rtv));auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);}
    void draw(){const float black[]{0,0,0,1};context->ClearRenderTargetView(rtv.Get(),black);context->Draw(3,0);}
    void present(bool extended=false,UINT flags=0){if(extended){ComPtr<IDXGISwapChain1> v;check(swap.As(&v));DXGI_PRESENT_PARAMETERS p{};check(v->Present1(0,flags,&p));}else check(swap->Present(0,flags));}
    ~Fixture(){context->ClearState();context->Flush();rtv.Reset();swap.Reset();context.Reset();device.Reset();DestroyWindow(window);}
};
Json control_arm(uint64_t id,const fs::path& path){return std::async(std::launch::async,[&]{return arm_capture(id,path);}).get();}
Json control_cancel(){return std::async(std::launch::async,[]{return cancel_capture();}).get();}
Json settled(){for(unsigned i=0;i<1000;++i){auto p=capture_progress();if(!p.value("busy",false))return p;Sleep(5);}throw std::runtime_error("Capture transaction did not settle");}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==2,"Expected new output directory");const auto out=fs::absolute(argv[1]);require(!fs::exists(out),"Test output directory must be new");fs::create_directories(out);Json results=Json::array(),races=Json::array(),faults=Json::array();
    {
        Fixture f;f.draw();f.present();require(!control_arm(UINT64_MAX,out/L"unknown.gpa_frame")["ok"],"Unknown swap accepted");
        for(unsigned i=0;i<3;++i){
            const auto path=out/(L"frame-"+std::to_wstring(i)+L".gpa_frame");require(control_arm(f.id,path)["ok"],"Could not arm capture");require(control_arm(f.id,out/L"busy.gpa_frame")["error"]=="busy","Duplicate capture not rejected");
            f.present(false,DXGI_PRESENT_TEST);require(capture_progress()["state"]=="Armed","TEST advanced armed boundary");
            f.draw();f.present(i==1);require(capture_progress()["state"]=="Capturing","First Present did not start capture");
            f.draw();f.present(false,DXGI_PRESENT_TEST);require(capture_progress()["state"]=="Capturing","TEST ended capture");f.present(i==1);
            auto result=settled();results.push_back(result);require(result["state"]=="Completed" && fs::exists(path),result.dump().c_str());require(control_cancel()["cancelled"]==false,"Published file was reported cancelled");
        }
        require(!control_arm(f.id,out/L"frame-0.gpa_frame")["ok"],"Existing capture accepted");
        require(control_arm(f.id,out/L"cancel-armed.gpa_frame")["ok"],"Cannot arm cancellation test");require(control_cancel()["cancelled"],"Armed cancellation failed");
        require(control_arm(f.id,out/L"cancel-recording.gpa_frame")["ok"],"Cannot arm active cancellation");f.present();f.draw();control_cancel();require(capture_progress()["busy"],"Cancelled GPU model released on control path");f.present();require(settled()["state"]=="Cancelled","Active cancellation not settled");
        require(control_arm(f.id,out/L"resize.gpa_frame")["ok"],"Cannot arm resize test");f.present();f.draw();f.context->OMSetRenderTargets(0,nullptr,nullptr);f.rtv.Reset();check(f.swap->ResizeBuffers(1,8,8,DXGI_FORMAT_R8G8B8A8_UNORM,0));require(settled()["state"]=="Failed","Resize did not reject frame");f.target();
        require(control_arm(f.id,out/L"unsupported.gpa_frame")["ok"],"Cannot arm unsupported test");f.present();f.draw();{ComPtr<ID3D11Texture2D> buffer;check(f.swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));std::array<UINT,64> pixels{};f.context->UpdateSubresource(buffer.Get(),0,nullptr,pixels.data(),32,256);}f.present();require(settled()["state"]=="Failed","Unsupported call was silently omitted");
        require(control_arm(f.id,out/L"boundary-gap.gpa_frame")["ok"],"Cannot arm boundary gap test");f.present();f.draw();capture_before_present(f.id,nullptr,0,false);f.draw();capture_after_present(f.id,nullptr,0,0,false,S_OK);require(settled()["state"]=="Failed","Context call inside Present boundary was omitted");
        for(unsigned i=0;i<12;++i){
            const auto path=out/(L"race-"+std::to_wstring(i)+L".gpa_frame");require(control_arm(f.id,path)["ok"],"Cannot arm publication race");f.present();f.draw();f.present();control_cancel();const auto result=settled();
            races.push_back(result);
            require((result["state"]=="Completed" && fs::exists(path)) || (result["state"]=="Cancelled" && !fs::exists(path)),"Cancellation/publication outcome is inconsistent");
        }
        for(auto hr:{DXGI_STATUS_OCCLUDED,DXGI_ERROR_DEVICE_REMOVED}){
            const auto path=out/(L"failed-present-"+std::to_wstring(uint32_t(hr))+L".gpa_frame");require(control_arm(f.id,path)["ok"],"Cannot arm injected Present result test");f.present();f.draw();capture_before_present(f.id,nullptr,0,false);capture_after_present(f.id,nullptr,0,0,false,hr);
            const auto failure=settled();faults.push_back(failure);require(failure["state"]=="Failed" && !fs::exists(path),"Unsuccessful Present status published output");
        }
        require(control_arm(f.id,out/L"missing-directory"/L"write.gpa_frame")["ok"],"Cannot arm disk failure test");f.present();f.draw();f.present();const auto disk=settled();faults.push_back(disk);require(disk["state"]=="Failed" && !fs::exists(out/L"missing-directory"/L"write.gpa_frame"),"Writer failure published output");
        require(control_arm(f.id,out/L"disconnect.gpa_frame")["ok"],"Cannot arm disconnect test");f.present();std::async(std::launch::async,[]{disconnect_capture();}).get();f.draw();f.present();require(settled()["state"]=="Cancelled","Disconnect did not cancel");f.draw();f.present();require(!control_arm(f.id,out/L"after-disconnect.gpa_frame")["ok"],"Disconnected service accepted request");
    }
    for(const auto* name:{L"busy",L"cancel-armed",L"cancel-recording",L"resize",L"unsupported",L"boundary-gap",L"disconnect",L"after-disconnect"})require(!fs::exists(out/(std::wstring(name)+L".gpa_frame")),"Failed capture published a file");
    for(const auto& entry:fs::directory_iterator(out))require(entry.path().extension()!=L".partial","Capture transaction leaked a partial file");
    require(object_status()["active"]==0 && metadata_status()["alive"]==0,"Capture session retained native objects");
    std::ofstream report(out/L"results.json");report<<Json{{"completed",results},{"publication_races",races},{"injected_failures",faults}}.dump(2);std::cout<<Json{{"passed",true},{"completed",3},{"scope","Actual swap-chain boundaries; internal control API"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
