// Real DX11 resources, controlled Present return values. This is a fault
// injection test, not evidence of actual screen presentation or device loss.
#include "capture_session.hpp"
#include "capture_requests.hpp"
#include "hud_control.hpp"
#include "objects.hpp"
#include "swap_chain.hpp"
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
using namespace flora::capture;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("DX11 HRESULT "+std::to_string(uint32_t(hr)));}
std::vector<uint8_t> pixels(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source){
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage;check(device->CreateTexture2D(&desc,nullptr,&stage));context->CopyResource(stage.Get(),source);
    D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));std::vector<uint8_t> result(size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y)memcpy(result.data()+size_t(y)*desc.Width*4,static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch,desc.Width*4);context->Unmap(stage.Get(),0);return result;
}
class FaultSwap final:public IDXGISwapChain {
    ULONG references_=1;
    ComPtr<IDXGISwapChain> real_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
public:
    HRESULT result=S_OK;
    bool fail_restore=false,fail_next_buffer=false,decorated=false;
    UINT interval=0,flags=0;
    explicit FaultSwap(IDXGISwapChain* real):real_(real){check(real_->GetDevice(IID_PPV_ARGS(&device_)));device_->GetImmediateContext(&context_);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid!=__uuidof(IUnknown) && iid!=__uuidof(IDXGIObject) && iid!=__uuidof(IDXGIDeviceSubObject) && iid!=__uuidof(IDXGISwapChain))return E_NOINTERFACE;
        *out=static_cast<IDXGISwapChain*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT options) override {
        interval=sync;flags=options;
        ComPtr<ID3D11Texture2D> buffer;check(real_->GetBuffer(0,IID_PPV_ARGS(&buffer)));const auto image=pixels(device_.Get(),context_.Get(),buffer.Get());
        decorated=false;for(size_t i=0;i<image.size();i+=4)if(image[i]!=32 || image[i+1]!=48 || image[i+2]!=96 || image[i+3]!=255){decorated=true;break;}
        if(fail_restore)fail_next_buffer=true;
        SetLastError(0x51a7);return result;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT slot,REFIID iid,void** out) override {
        if(fail_next_buffer){fail_next_buffer=false;if(out)*out=nullptr;return E_FAIL;}return real_->GetBuffer(slot,iid,out);
    }
#define FORWARD(type,name,args,call) type STDMETHODCALLTYPE name args override{return real_->name call;}
    FORWARD(HRESULT,SetPrivateData,(REFGUID key,UINT size,const void* data),(key,size,data))
    FORWARD(HRESULT,SetPrivateDataInterface,(REFGUID key,const IUnknown* object),(key,object))
    FORWARD(HRESULT,GetPrivateData,(REFGUID key,UINT* size,void* data),(key,size,data))
    FORWARD(HRESULT,GetParent,(REFIID iid,void** out),(iid,out))
    FORWARD(HRESULT,GetDevice,(REFIID iid,void** out),(iid,out))
    FORWARD(HRESULT,SetFullscreenState,(BOOL state,IDXGIOutput* output),(state,output))
    FORWARD(HRESULT,GetFullscreenState,(BOOL* state,IDXGIOutput** output),(state,output))
    FORWARD(HRESULT,GetDesc,(DXGI_SWAP_CHAIN_DESC* desc),(desc))
    FORWARD(HRESULT,ResizeBuffers,(UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT options),(count,width,height,format,options))
    FORWARD(HRESULT,ResizeTarget,(const DXGI_MODE_DESC* desc),(desc))
    FORWARD(HRESULT,GetContainingOutput,(IDXGIOutput** output),(output))
    FORWARD(HRESULT,GetFrameStatistics,(DXGI_FRAME_STATISTICS* statistics),(statistics))
    FORWARD(HRESULT,GetLastPresentCount,(UINT* count),(count))
#undef FORWARD
};
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
int wmain(int argc,wchar_t** argv){try{
    require(argc>=2 && argc<=4,"Expected a new output directory and optional --hardware/--escape");bool hardware=false,escape=false;
    for(int i=2;i<argc;++i){const std::wstring option=argv[i];if(option==L"--hardware")hardware=true;else if(option==L"--escape")escape=true;else throw std::runtime_error("Unknown HUD guard test option");}
    const auto out=fs::absolute(argv[1]);require(!fs::exists(out),"Output already exists");fs::create_directories(out);
    configure_capture_requests(out);hud::Settings settings;settings.mode_key={};settings.capture_key={};configure_hud(settings);
    WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"FloraHudFaultOracle";require(RegisterClassW(&cls)!=0,"Window class failed");
    HWND window=CreateWindowW(cls.lpszClassName,L"Flora HUD fault oracle",WS_POPUP,0,0,800,600,nullptr,nullptr,cls.hInstance,nullptr);require(window!=nullptr,"Window failed");
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=800;desc.BufferDesc.Height=600;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferCount=2;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_SEQUENTIAL;desc.Windowed=TRUE;desc.OutputWindow=window;
    ComPtr<ID3D11Device> native_device;ComPtr<ID3D11DeviceContext> native_context;ComPtr<IDXGISwapChain> real;
    check(D3D11CreateDeviceAndSwapChain(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&desc,&real,&native_device,nullptr,&native_context));
    ComPtr<ID3D11InfoQueue> info;check(native_device.As(&info));Json cases=Json::array();
    {
        ComPtr<ID3D11Device> device=native_device;ComPtr<ID3D11DeviceContext> context=native_context;
        wrap_object(device.GetAddressOf(),"HUD fault device");wrap_object(context.GetAddressOf(),"HUD fault context");
        auto* fault=new FaultSwap(real.Get());ComPtr<IDXGISwapChain> swap;swap.Attach(fault);wrap_swap_chain(swap.GetAddressOf());
        ComPtr<ID3D11Texture2D> buffer;check(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)));ComPtr<ID3D11RenderTargetView> view;check(device->CreateRenderTargetView(buffer.Get(),nullptr,&view));
        const float color[]{32.f/255,48.f/255,96.f/255,1};const auto clear=[&]{context->ClearRenderTargetView(view.Get(),color);};
        for(unsigned i=0;i<3;++i){clear();check(swap->Present(0,0));}
        const auto id=swap_chain_status()["active"][0]["id"].get<uint64_t>();
        std::vector<uint8_t> clean(800*600*4);for(size_t i=0;i<clean.size();i+=4){clean[i]=32;clean[i+1]=48;clean[i+2]=96;clean[i+3]=255;}
        const auto exact=[&]{require(pixels(device.Get(),context.Get(),buffer.Get())==clean,"Fault left HUD pixels in application storage");};exact();
        const auto mapped_present=[&](bool should_draw){
            clear();check(swap->Present(0,0));require(fault->decorated==should_draw,"Mapped-resource HUD guard drew or skipped incorrectly");exact();
            const auto status=hud_status();require(status["swap_chains"].size()==1,"Mapped-resource swap missing");
            require(status["swap_chains"][0]["reason"]==(should_draw?"":"HUD skips active texture or external mappings"),"Mapped-resource HUD reason missing");
        };
        // A Map borrowed from the application remains valid across Present.
        // Inspect GPU bytes after Unmap rather than reading write-combined CPU
        // memory. Private HUD work must neither end nor modify the mapping.
        {
            D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DYNAMIC;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&td,nullptr,&texture));D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(texture.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map));
            for(UINT y=0;y<8;++y)memset(static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch,0x5a,32);
            for(unsigned i=0;i<3;++i){mapped_present(false);require(metadata_status()["active_mapping_count"]==1,"HUD changed application texture mapping lifetime");}
            context->Unmap(texture.Get(),0);require(pixels(device.Get(),context.Get(),texture.Get())==std::vector<uint8_t>(256,0x5a),"HUD changed mapped texture bytes");mapped_present(true);
            cases.push_back({{"dynamic_texture_map",true},{"skipped_presents",3},{"mapped_bytes_exact",true},{"resumed_after_unmap",true}});
        }
        {
            D3D11_BUFFER_DESC bd{};bd.ByteWidth=128;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> buffer,stage;check(device->CreateBuffer(&bd,nullptr,&buffer));D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map));memset(map.pData,0x6b,128);
            for(unsigned i=0;i<3;++i){mapped_present(true);require(metadata_status()["active_mapping_count"]==1,"HUD changed application buffer mapping lifetime");}
            context->Unmap(buffer.Get(),0);bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;check(device->CreateBuffer(&bd,nullptr,&stage));context->CopyResource(stage.Get(),buffer.Get());check(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map));const std::vector<uint8_t> expected(128,0x6b);const bool unchanged=memcmp(map.pData,expected.data(),expected.size())==0;context->Unmap(stage.Get(),0);require(unchanged,"HUD changed mapped buffer bytes");
            cases.push_back({{"dynamic_buffer_map",true},{"decorated_presents",3},{"mapped_bytes_exact",true}});
        }
        {
            D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=8;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&td,nullptr,&texture));ComPtr<IDXGISurface> surface;check(texture.As(&surface));DXGI_MAPPED_RECT map{};check(surface->Map(&map,DXGI_MAP_WRITE));for(UINT y=0;y<8;++y)memset(map.pBits+size_t(y)*map.Pitch,0x7c,32);
            for(unsigned i=0;i<3;++i){mapped_present(false);require(has_external_mapping(),"HUD ended application DXGI mapping");}
            check(surface->Unmap());require(!has_external_mapping(),"DXGI mapping did not end");require(pixels(device.Get(),context.Get(),texture.Get())==std::vector<uint8_t>(256,0x7c),"HUD changed DXGI mapped bytes");mapped_present(true);
            cases.push_back({{"dxgi_surface_map",true},{"skipped_presents",3},{"mapped_bytes_exact",true},{"resumed_after_unmap",true}});
        }
        unsigned ordinal=0;
        // Exercise the control-thread cancellation path with HUD rendering and
        // a native-resource failure facade in the same Present transaction.
        for(unsigned mode=0;mode<3;++mode){
            const auto path=out/(L"cancel-"+std::to_wstring(mode)+L".gpa_frame");fault->result=S_OK;require(arm_capture(id,path).value("ok",false),"Cannot arm HUD cancellation");
            if(mode){clear();check(swap->Present(0,0));require(capture_progress()["state"]=="Capturing","HUD cancellation did not enter recording");clear();}
            const auto cancel=std::async(std::launch::async,[]{return cancel_capture();}).get();require(cancel.value("ok",false),"Control cancellation failed");
            if(mode)require(capture_progress()["busy"],"Control cancellation released GPU state on the wrong thread");
            fault->result=mode==2?DXGI_ERROR_DEVICE_REMOVED:S_OK;const auto result=swap->Present(2,0);const auto last=GetLastError();
            require(result==fault->result && last==0x51a7 && fault->decorated,"Cancellation changed native result or skipped HUD");exact();const auto progress=capture_progress();
            require(progress["state"]=="Cancelled" && !progress["busy"].get<bool>() && !fs::exists(path),"Cancelled HUD frame was published or retained");
            require(hud_status()["message"]=="Cancelled","HUD cancellation feedback missing");cases.push_back({{"cancel_phase",mode?"recording":"armed"},{"injected_device_removed",mode==2},{"actual_return",uint32_t(result)},{"last_error",last},{"exact_application_pixels",true},{"progress",progress}});
        }
        fault->result=S_OK;{
            const auto path=out/L"missing-directory"/L"writer.gpa_frame";require(arm_capture(id,path).value("ok",false),"Cannot arm HUD writer failure");clear();check(swap->Present(0,0));clear();check(swap->Present(0,0));require(fault->decorated,"Writer-failure boundary did not render HUD");
            const auto deadline=GetTickCount64()+5000;while(capture_progress()["busy"].get<bool>() && GetTickCount64()<deadline)Sleep(1);const auto progress=capture_progress();
            require(progress["state"]=="Failed" && !progress["busy"].get<bool>() && !fs::exists(path),"HUD writer failure was not terminal");exact();
            cases.push_back({{"writer_missing_directory",true},{"exact_application_pixels",true},{"progress",progress}});
        }
        for(const auto result:{DXGI_STATUS_OCCLUDED,DXGI_ERROR_DEVICE_REMOVED,DXGI_ERROR_DEVICE_RESET,DXGI_ERROR_WAS_STILL_DRAWING}){
            const auto path=out/(L"fault-"+std::to_wstring(ordinal++)+L".gpa_frame");
            require(arm_capture(id,path).value("ok",false),"Cannot arm failure case");fault->result=S_OK;clear();check(swap->Present(0,0));require(capture_progress()["state"]=="Capturing","Capture did not start");clear();
            fault->result=result;const auto returned=swap->Present(2,0);const auto last=GetLastError();
            require(returned==result && last==0x51a7,"HUD changed native failure HRESULT or LastError");require(fault->interval==2 && fault->flags==0,"HUD changed Present arguments");require(fault->decorated,"No HUD was drawn before injected failure");
            const auto progress=capture_progress();require(progress["state"]=="Failed" && !progress["busy"].get<bool>() && !fs::exists(path),"Failed Present published a capture");exact();
            require(object_status().value("interface_gaps",0ull)==0,"Ordinary failed Present disabled forwarding");cases.push_back({{"injected_hresult",uint32_t(result)},{"actual_return",uint32_t(returned)},{"last_error",last},{"decorated",true},{"exact_application_pixels",true},{"progress",progress}});
        }
        if(escape){
            fault->result=S_OK;clear();check(swap->Present(0,0));const auto path=out/L"interface-escape.gpa_frame";require(arm_capture(id,path).value("ok",false),"Cannot arm interface guard");
            ComPtr<ID3D11Debug> native_debug,returned_debug;check(native_device.As(&native_debug));const auto hr=device.As(&returned_debug);check(hr);require(returned_debug==native_debug && returned_debug->GetFeatureMask()==native_debug->GetFeatureMask(),"Unsupported native interface did not forward transparently");
            const auto gap=object_status()["last_gap"];require(gap["api"]=="QueryInterface" && gap["reason"]=="Interface forwarding is not implemented" && !gap["iid"].get<std::string>().empty(),"Escaped IID was not reported");
            clear();const auto result=swap->Present(1,0);const auto last=GetLastError();require(result==S_OK && last==0x51a7 && !fault->decorated,"Escaped interface changed result or allowed HUD drawing");exact();
            require(hud_status()["swap_chains"][0]["reason"]=="HUD skips untracked interfaces","Escaped-interface HUD reason missing");const auto progress=capture_progress();require(progress["state"]=="Failed" && !fs::exists(path),"Interface escape published armed frame");
            require(submit_capture_request(id,"12345678-1234-4234-8234-123456789abc")["error"]=="capture_capability_incomplete","Interface escape allowed future capture");
            cases.push_back({{"native_debug_interface_escape",true},{"native_interface_forwarded",true},{"gap",gap},{"hud_skipped",true},{"exact_application_pixels",true},{"progress",progress}});
        }else{
        fault->result=S_OK;clear();check(swap->Present(0,0));const auto path=out/L"restore-failure.gpa_frame";require(arm_capture(id,path).value("ok",false),"Cannot arm restoration failure");clear();check(swap->Present(0,0));clear();fault->fail_restore=true;
        const auto returned=swap->Present(1,0);const auto last=GetLastError();require(returned==S_OK && last==0x51a7,"Internal restoration error replaced application result");require(fault->decorated,"Restoration failure never drew HUD");
        const auto progress=capture_progress();require(progress["state"]=="Failed" && !fs::exists(path),"Failed restoration published a capture");require(object_status().value("interface_gaps",0ull)>0,"Restoration failure was silent");exact();cases.push_back({{"restore_get_buffer_failure",true},{"actual_return",uint32_t(returned)},{"last_error",last},{"exact_application_pixels",true},{"progress",progress}});
        const auto rejected=submit_capture_request(id,"12345678-1234-4234-8234-123456789abc");require(!rejected.value("ok",false) && rejected["error"]=="capture_capability_incomplete","Shared capture entry ignored restoration failure");
        fault->fail_restore=false;clear();check(swap->Present(0,0));require(!fault->decorated,"HUD ignored a restoration capability gap");exact();
        }
        disconnect_hud();disconnect_capture_requests();context->ClearState();
    }
    for(const auto& entry:fs::directory_iterator(out))require(entry.path().extension()!=L".gpa_frame" && entry.path().extension()!=L".partial","Failure leaked a published or partial frame");
    require(object_status()["active"]==0,"Fault cleanup retained wrapped objects");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T length=0;check(info->GetMessage(i,nullptr,&length));std::vector<uint8_t> bytes(length);auto* message=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());check(info->GetMessage(i,message,&length));if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING)throw std::runtime_error(message->pDescription);}
    info.Reset();real.Reset();native_context.Reset();native_device.Reset();DestroyWindow(window);
    require(metadata_status()["alive"]==0,"Fault cleanup retained native metadata lifetimes");
    std::ofstream file(out/L"results.json");file<<Json{{"scope","Injected Present/GetBuffer results; real DX11 resources; no actual device removal or screen presentation"},{"driver",hardware?"hardware":"WARP"},{"cases",cases},{"no_frame_or_partial_files",true},{"no_debug_warnings",true},{"wrapped_objects_released",true},{"future_capture_rejected",true}}.dump(2);
    std::cout<<cases.size()<<" HUD mapping/interface/failure cases preserve application bytes/results and publish no frame; native resources and Debug Layer verified\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
