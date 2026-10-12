#include "hud_control.hpp"
#include "capture_requests.hpp"
#include "capture_session.hpp"
#include "objects.hpp"
#include "hud_pixels.hpp"
#include <wrl/client.h>
#include <objbase.h>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace flora::capture {
namespace {
uint64_t now_ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Control {
    std::mutex mutex;
    std::condition_variable ready;
    hud::Settings settings;
    bool mode_edge=false,capture_edge=false;
    std::map<DWORD,HHOOK> hooks;
    std::string input_error;
    std::map<uint64_t,hud::FrameClock> clocks;
    hud::Message message;
    Json last_request;
    std::string last_progress;
    bool connected=false,pending=false,processing=false;
    uint64_t sequence=0;
    struct Visibility {bool visible=false;std::string reason;uint64_t last_present=0;};
    std::map<uint64_t,Visibility> visibility;
};
Control& control(){static auto* value=new Control;return *value;}
LRESULT CALLBACK keyboard_message(int code,WPARAM removed,LPARAM parameter){
    const auto error=GetLastError();
    try{if(code==HC_ACTION && removed==PM_REMOVE){const auto& message=*reinterpret_cast<const MSG*>(parameter);
        if((message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN) && !(message.lParam&(LPARAM(1)<<30))){
            DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            if(foreground==GetCurrentProcessId()){
                const auto matches=[&](const hud::Hotkey& key){return key.key && message.wParam==key.key && bool(GetKeyState(VK_CONTROL)&0x8000)==key.control && bool(GetKeyState(VK_SHIFT)&0x8000)==key.shift && bool(GetKeyState(VK_MENU)&0x8000)==key.alt && !(GetKeyState(VK_LWIN)&0x8000) && !(GetKeyState(VK_RWIN)&0x8000);};
                auto& c=control();std::lock_guard lock(c.mutex);if(c.connected){if(matches(c.settings.mode_key))c.mode_edge=true;if(matches(c.settings.capture_key))c.capture_edge=true;}
            }
        }
    }}catch(...){}
    SetLastError(error);return CallNextHookEx(nullptr,code,removed,parameter);
}
std::string uuid(){GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot create capture request ID");
    char text[40]{};sprintf_s(text,"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",id.Data1,id.Data2,id.Data3,id.Data4[0],id.Data4[1],id.Data4[2],id.Data4[3],id.Data4[4],id.Data4[5],id.Data4[6],id.Data4[7]);return text;}
void feedback(Control& c,const Json& result){
    c.last_request=result;
    if(!result.value("ok",false)){
        const auto error=result.value("error",std::string("Capture failed"));
        const auto text=error=="busy"?"Busy":error=="swap_chain_selection_required"?"Select swap chain":"Capture failed: "+error;
        c.message.update("request-"+std::to_string(++c.sequence),text,GetTickCount64(),8000);
    }
}
void worker(){
    auto& c=control();
    for(;;){
        {std::unique_lock lock(c.mutex);c.ready.wait(lock,[&]{return c.pending || !c.connected;});if(!c.connected)return;c.pending=false;c.processing=true;}
        Json result;
        try{result=submit_capture_request(selected_capture_swap(),uuid());}catch(const std::exception& e){result={{"ok",false},{"error",e.what()}};}catch(...){result={{"ok",false},{"error","Capture request failed"}};}
        {std::lock_guard lock(c.mutex);c.processing=false;feedback(c,result);}
    }
}
void progress_message(Control& c,const Json& progress){
    const auto phase=progress.value("state",std::string());
    const auto identity=progress.dump();if(identity==c.last_progress)return;c.last_progress=identity;
    const auto time=GetTickCount64();
    if(phase=="Armed" || phase=="Capturing")c.message.update(identity,phase,time,0);
    else if(phase=="Finalizing")c.message.update(identity,"Saving",time,0);
    else if(phase=="Completed"){const auto path=progress.value("file",std::string());const auto slash=path.find_last_of("/\\");c.message.update(identity,"Saved: "+path.substr(slash==std::string::npos?0:slash+1),time,4000);}
    else if(phase=="Cancelled")c.message.update(identity,"Cancelled",time,4000);
    else if(phase=="Failed")c.message.update(identity,"Capture failed: "+progress.value("error",std::string()),time,8000);
}
}
void configure_hud(const hud::Settings& settings){
    settings.validate();auto& c=control();std::lock_guard lock(c.mutex);if(c.connected)throw std::logic_error("HUD already configured");
    c.settings=settings;c.connected=true;
    try{std::thread([]{try{worker();}catch(...){auto& state=control();std::lock_guard lock(state.mutex);state.connected=false;state.pending=false;state.processing=false;}}).detach();}catch(...){c.connected=false;throw;}
}
void disconnect_hud() noexcept {
    try{auto& c=control();std::map<DWORD,HHOOK> hooks;{std::lock_guard lock(c.mutex);c.connected=false;c.pending=false;c.mode_edge=c.capture_edge=false;hooks.swap(c.hooks);}for(const auto& [id,hook]:hooks){(void)id;UnhookWindowsHookEx(hook);}c.ready.notify_one();}catch(...){}
}
void hud_input(UINT flags,IDXGISwapChain* native) noexcept {
    if(flags&DXGI_PRESENT_TEST)return;const auto error=GetLastError();
    try{
        DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
        NativeCall internal;DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(native->GetDesc(&desc))){SetLastError(error);return;}
        DWORD window_process=0;const auto thread=GetWindowThreadProcessId(desc.OutputWindow,&window_process);
        auto& c=control();std::lock_guard lock(c.mutex);if(!c.connected){SetLastError(error);return;}
        if(thread && window_process==GetCurrentProcessId() && !c.hooks.contains(thread) && (c.settings.mode_key.key || c.settings.capture_key.key)){
            HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&keyboard_message),&module);
            if(auto hook=SetWindowsHookExW(WH_GETMESSAGE,keyboard_message,module,thread)){c.hooks.emplace(thread,hook);c.input_error.clear();}
            else c.input_error="Cannot observe target keyboard messages: "+std::to_string(GetLastError());
        }
        const bool focused=foreground==GetCurrentProcessId();
        if(focused && c.mode_edge)c.settings.mode=c.settings.mode==hud::Mode::Compact?hud::Mode::Detailed:c.settings.mode==hud::Mode::Detailed?hud::Mode::Hidden:hud::Mode::Compact;
        if(focused && c.capture_edge){
            if(c.pending || c.processing)feedback(c,{{"ok",false},{"error","busy"}});
            else{c.pending=true;c.ready.notify_one();}
        }
        c.mode_edge=c.capture_edge=false;
    }catch(...){}SetLastError(error);
}
void hud_presented(uint64_t swap,UINT flags,HRESULT result) noexcept {
    if(flags&DXGI_PRESENT_TEST)return;const auto error=GetLastError();
    try{auto& c=control();std::lock_guard lock(c.mutex);if(c.connected)c.clocks[swap].present(now_ns(),result==S_OK,false);if(result!=S_OK)c.visibility[swap].visible=false;}catch(...){}SetLastError(error);
}
void hud_removed(uint64_t swap) noexcept {const auto error=GetLastError();try{auto& c=control();std::lock_guard lock(c.mutex);c.clocks.erase(swap);c.visibility.erase(swap);}catch(...){}SetLastError(error);}
void hud_feedback(const Json& result){auto& c=control();std::lock_guard lock(c.mutex);feedback(c,result);}
Json hud_status(){
    const auto progress=capture_progress();const auto selected=selected_capture_swap();auto& c=control();std::lock_guard lock(c.mutex);progress_message(c,progress);
    Json rows=Json::array();const auto time=now_ns();bool visible=false;
    for(const auto& [id,clock]:c.clocks){const auto f=clock.value(time);const auto v=c.visibility[id];const bool shown=c.connected && c.settings.mode!=hud::Mode::Hidden && v.visible && GetTickCount64()-v.last_present<2000;visible|=shown;rows.push_back({{"swap_chain",id},{"frames",f.frames},{"visible",shown},{"reason",v.reason},{"fps",f.current?Json(*f.current):Json()},{"minimum",f.minimum?Json(*f.minimum):Json()},{"maximum",f.maximum?Json(*f.maximum):Json()}});}
    return {{"mode",hud::mode_name(c.settings.mode)},{"connected",c.connected},{"visible",visible},{"selected_swap_chain",selected},
        {"reason",!c.connected?"Disconnected":c.settings.mode==hud::Mode::Hidden?"Hidden":visible?"":"See per-swap HUD reason"},
        {"hud_hotkey",hud::hotkey_name(c.settings.mode_key)},{"capture_hotkey",hud::hotkey_name(c.settings.capture_key)},
        {"input_reason",c.input_error},{"input_threads",c.hooks.size()},{"input_profile","Target-thread keyboard messages; raw-input-only keys are not supported"},
        {"request_pending",c.pending || c.processing},{"message",c.message.value(GetTickCount64())},{"last_request",c.last_request},{"swap_chains",rows}};
}
struct HudDraw::Impl {
    std::unique_ptr<hud::PixelTransaction> pixels;
    uint64_t id=0;
    bool drawn=false;
    std::string reason;
};
HudDraw::HudDraw()=default;
HudDraw::~HudDraw(){const auto error=GetLastError();restore();impl_.reset();SetLastError(error);}
void HudDraw::begin(uint64_t swap,IDXGISwapChain* native,UINT flags,bool partial) noexcept {
    if(flags&DXGI_PRESENT_TEST)return;const auto error=GetLastError();
    try{
        auto& c=control();hud::Panel panel;
        {const auto progress=capture_progress();std::lock_guard lock(c.mutex);if(!c.connected || c.settings.mode==hud::Mode::Hidden){SetLastError(error);return;}progress_message(c,progress);
            panel.mode=c.settings.mode;panel.fps=c.clocks[swap].value(now_ns());panel.swap=swap;
            const auto wide=[](const std::string& text){const auto length=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);std::wstring result(length,L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),result.data(),length);return result;};
            panel.mode_key=wide(hud::hotkey_name(c.settings.mode_key));panel.capture_key=wide(hud::hotkey_name(c.settings.capture_key));panel.message=wide(c.message.value(GetTickCount64()));}
        impl_=std::make_unique<Impl>();impl_->id=swap;
        NativeCall internal;
        DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(native->GetDesc(&desc)))throw std::runtime_error("HUD swap descriptor unavailable");
        if(desc.BufferCount!=1 || (desc.SwapEffect!=DXGI_SWAP_EFFECT_DISCARD && desc.SwapEffect!=DXGI_SWAP_EFFECT_SEQUENTIAL))throw std::runtime_error("Multi-buffer HUD pixel restoration is unverified");
        Microsoft::WRL::ComPtr<ID3D11Device> device;if(FAILED(native->GetDevice(IID_PPV_ARGS(&device))))throw std::runtime_error("HUD requires a native DX11 device");
        if(!(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED))throw std::runtime_error("HUD concurrency isolation requires a SINGLETHREADED device in this profile");
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        Microsoft::WRL::ComPtr<IUnknown> identity;context.As(&identity);const auto record=find_native_record(identity.Get());
        if(!record)throw std::runtime_error("HUD immediate context is untracked");
        {std::lock_guard lock(record->mutex);if(!record->active_queries.empty())throw std::runtime_error("HUD skips active queries");}
        if(object_status().value("interface_gaps",0ull) || has_external_mapping(true))throw std::runtime_error("HUD skips untracked interfaces or mapped resources");
        Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;context->GetPredication(&predicate,&value);if(predicate)throw std::runtime_error("HUD skips active predication");
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_desc{};
        if(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapter_desc)))panel.adapter=adapter_desc.Description;
        const auto level=device->GetFeatureLevel();panel.device=L"DX "+std::to_wstring((unsigned(level)>>12)&15)+L"."+std::to_wstring((unsigned(level)>>8)&15);
        Microsoft::WRL::ComPtr<IUnknown> device_identity;device.As(&device_identity);
        if(const auto record_device=find_native_record(device_identity.Get())){std::lock_guard lock(record_device->mutex);if(record_device->driver_type){const wchar_t* label=L"UNKNOWN";switch(*record_device->driver_type){case D3D_DRIVER_TYPE_HARDWARE:label=L"HW";break;case D3D_DRIVER_TYPE_WARP:label=L"WARP";break;case D3D_DRIVER_TYPE_REFERENCE:label=L"REF";break;case D3D_DRIVER_TYPE_SOFTWARE:label=L"SW";break;case D3D_DRIVER_TYPE_NULL:label=L"NULL";break;default:break;}panel.device+=L" ["+std::wstring(label)+L"]";}}
        impl_->pixels=std::make_unique<hud::PixelTransaction>();
        impl_->drawn=impl_->pixels->begin(native,panel,flags,partial,impl_->reason);
    }catch(const std::exception& e){if(impl_)impl_->reason=e.what();}catch(...){if(impl_)impl_->reason="HUD setup failed";}
    if(impl_)try{auto& c=control();std::lock_guard lock(c.mutex);c.visibility[swap]={impl_->drawn,impl_->reason,GetTickCount64()};}catch(...){}
    SetLastError(error);
}
bool HudDraw::restore() noexcept {
    if(!impl_ || !impl_->pixels)return true;const auto error=GetLastError();bool clean=true;
    {NativeCall internal;clean=impl_->pixels->restore(impl_->reason);impl_->pixels.reset();}
    if(!clean){object_gap("HUD restore",__uuidof(IDXGISwapChain),"HUD pixel restoration failed; capture disabled");try{auto& c=control();std::lock_guard lock(c.mutex);c.visibility[impl_->id]={false,impl_->reason,GetTickCount64()};}catch(...){}}
    SetLastError(error);return clean;
}
}
