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
#include <vector>
#include <set>

namespace flora::capture {
namespace {
uint64_t now_ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Control {
    std::mutex mutex;
    std::condition_variable ready;
    hud::Settings settings;
    bool mode_edge=false,capture_edge=false;
    std::map<DWORD,HHOOK> hooks;
    std::map<DWORD,hud::RawKeyboard> raw_keys;
    uint64_t raw_events=0,legacy_events=0;
    bool raw_focused=false;
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
std::array<bool,256> raw_key_snapshot(const hud::Settings& settings){
    std::array<bool,256> keys{};
    for(const auto key:{UINT(VK_LSHIFT),UINT(VK_RSHIFT),UINT(VK_LCONTROL),UINT(VK_RCONTROL),UINT(VK_LMENU),UINT(VK_RMENU),UINT(VK_LWIN),UINT(VK_RWIN),settings.mode_key.key,settings.capture_key.key})if(key)keys[key]=(GetAsyncKeyState(int(key))&0x8000)!=0;
    return keys;
}
void raw_focus(Control& c,bool focused){
    if(c.raw_focused==focused)return;c.raw_focused=focused;
    const auto keys=raw_key_snapshot(c.settings);for(auto& [thread,keyboard]:c.raw_keys){(void)thread;keyboard.seed(keys,focused,c.settings);}
}
void input_failure(const char* reason) noexcept {try{auto& c=control();std::lock_guard lock(c.mutex);c.input_error=reason;}catch(...){}}
std::vector<RAWINPUTDEVICE> keyboard_registrations(){
    UINT count=0;if(GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE))==UINT(-1) || count>256)throw std::runtime_error("Cannot inspect raw keyboard registrations");
    std::vector<RAWINPUTDEVICE> devices(count);if(count && GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE))==UINT(-1))throw std::runtime_error("Raw keyboard registrations changed");
    devices.resize(count);std::erase_if(devices,[](const auto& device){return device.usUsagePage!=1 || device.usUsage!=6;});return devices;
}
LRESULT CALLBACK keyboard_message(int code,WPARAM removed,LPARAM parameter){
    const auto error=GetLastError();
    try{if(code==HC_ACTION && removed==PM_REMOVE){const auto& message=*reinterpret_cast<const MSG*>(parameter);
        if((message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN) && !(message.lParam&(LPARAM(1)<<30))){
            DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            if(foreground==GetCurrentProcessId()){
                const auto matches=[&](const hud::Hotkey& key){return key.key && message.wParam==key.key && bool(GetKeyState(VK_CONTROL)&0x8000)==key.control && bool(GetKeyState(VK_SHIFT)&0x8000)==key.shift && bool(GetKeyState(VK_MENU)&0x8000)==key.alt && !(GetKeyState(VK_LWIN)&0x8000) && !(GetKeyState(VK_RWIN)&0x8000);};
                auto& c=control();std::lock_guard lock(c.mutex);if(c.connected){++c.legacy_events;if(matches(c.settings.mode_key))c.mode_edge=true;if(matches(c.settings.capture_key))c.capture_edge=true;}
            }
        }else if(message.message==WM_INPUT){
            // The application still owns this handle and its cleanup. Reading
            // does not change its registration or consume/replace its message.
            RAWINPUT input{};UINT bytes=sizeof(input);const auto read=GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam),RID_INPUT,&input,&bytes,sizeof(RAWINPUTHEADER));
            if(read!=UINT(-1) && read>=sizeof(RAWINPUTHEADER)+sizeof(RAWKEYBOARD) && input.header.dwType==RIM_TYPEKEYBOARD){
                bool raw_only=false;for(const auto& device:keyboard_registrations())raw_only|=(device.dwFlags&RIDEV_NOLEGACY)==RIDEV_NOLEGACY;
                if(raw_only){
                    const auto& key=input.data.keyboard;UINT vk=key.VKey;
                    // Injected/generic keys may have no make code. Windows'
                    // generic Shift maps to the left variant in that case.
                    if(vk==VK_SHIFT)vk=key.MakeCode?MapVirtualKeyW(key.MakeCode,MAPVK_VSC_TO_VK_EX):VK_LSHIFT;
                    else if(vk==VK_CONTROL)vk=(key.Flags&RI_KEY_E0)?VK_RCONTROL:VK_LCONTROL;
                    else if(vk==VK_MENU)vk=(key.Flags&RI_KEY_E0)?VK_RMENU:VK_LMENU;
                    DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
                    auto& c=control();std::lock_guard lock(c.mutex);if(c.connected){++c.raw_events;const bool focused=foreground==GetCurrentProcessId();raw_focus(c,focused);const auto action=c.raw_keys[GetCurrentThreadId()].event(vk,!(key.Flags&RI_KEY_BREAK),focused,c.settings);c.mode_edge|=action.mode;c.capture_edge|=action.capture;}
                }
            }
        }
    }}catch(const std::exception& e){input_failure(e.what());}catch(...){input_failure("Cannot observe keyboard input");}
    SetLastError(error);return CallNextHookEx(nullptr,code,removed,parameter);
}
std::string uuid(){GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot create capture request ID");
    char text[40]{};sprintf_s(text,"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",id.Data1,id.Data2,id.Data3,id.Data4[0],id.Data4[1],id.Data4[2],id.Data4[3],id.Data4[4],id.Data4[5],id.Data4[6],id.Data4[7]);return text;}
void feedback(Control& c,const Json& result){
    c.last_request=result;
    if(!result.value("ok",false)){
        const auto error=result.value("error",std::string("Capture failed"));
        const auto text=error=="busy"?"Busy":error=="swap_chain_selection_required"?"Select swap chain":hud::failure_summary(error);
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
    else if(phase=="Failed")c.message.update(identity,hud::failure_summary(progress.value("error",std::string())),time,8000);
}
}
void configure_hud(const hud::Settings& settings){
    settings.validate();auto& c=control();std::lock_guard lock(c.mutex);if(c.connected)throw std::logic_error("HUD already configured");
    c.settings=settings;c.connected=true;
    try{std::thread([]{try{worker();}catch(...){auto& state=control();std::lock_guard lock(state.mutex);state.connected=false;state.pending=false;state.processing=false;}}).detach();}catch(...){c.connected=false;throw;}
}
void disconnect_hud() noexcept {
    try{auto& c=control();std::map<DWORD,HHOOK> hooks;{std::lock_guard lock(c.mutex);c.connected=false;c.pending=false;c.mode_edge=c.capture_edge=false;hooks.swap(c.hooks);c.raw_keys.clear();}for(const auto& [id,hook]:hooks){(void)id;UnhookWindowsHookEx(hook);}c.ready.notify_one();}catch(...){}
}
bool hud_connected() noexcept {try{auto& c=control();std::lock_guard lock(c.mutex);return c.connected;}catch(...){return false;}}
void hud_input(UINT flags,IDXGISwapChain* native) noexcept {
    if(flags&DXGI_PRESENT_TEST)return;const auto error=GetLastError();
    if(!hud_connected()){SetLastError(error);return;}
    try{
        DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
        NativeCall internal;DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(native->GetDesc(&desc))){SetLastError(error);return;}
        DWORD window_process=0;const auto thread=GetWindowThreadProcessId(desc.OutputWindow,&window_process);
        std::set<DWORD> threads;if(thread && window_process==GetCurrentProcessId())threads.insert(thread);
        for(const auto& device:keyboard_registrations())if(device.hwndTarget){DWORD pid=0;const auto input_thread=GetWindowThreadProcessId(device.hwndTarget,&pid);if(input_thread && pid==GetCurrentProcessId())threads.insert(input_thread);}
        auto& c=control();std::lock_guard lock(c.mutex);if(!c.connected){SetLastError(error);return;}
        raw_focus(c,foreground==GetCurrentProcessId());
        for(const auto input_thread:threads)if(!c.hooks.contains(input_thread) && (c.settings.mode_key.key || c.settings.capture_key.key)){
            HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&keyboard_message),&module);
            if(auto hook=SetWindowsHookExW(WH_GETMESSAGE,keyboard_message,module,input_thread)){
                c.hooks.emplace(input_thread,hook);c.input_error.clear();c.raw_keys[input_thread].seed(raw_key_snapshot(c.settings),foreground==GetCurrentProcessId(),c.settings);
            }
            else c.input_error="Cannot observe target keyboard messages: "+std::to_string(GetLastError());
        }
        const bool focused=foreground==GetCurrentProcessId();
        if(focused && c.mode_edge)c.settings.mode=c.settings.mode==hud::Mode::Compact?hud::Mode::Detailed:c.settings.mode==hud::Mode::Detailed?hud::Mode::Hidden:hud::Mode::Compact;
        if(focused && c.capture_edge){
            if(c.pending || c.processing)feedback(c,{{"ok",false},{"error","busy"}});
            else{c.pending=true;c.ready.notify_one();}
        }
        c.mode_edge=c.capture_edge=false;
    }catch(const std::exception& e){input_failure(e.what());}catch(...){input_failure("Cannot install keyboard observer");}SetLastError(error);
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
        {"input_reason",c.input_error},{"input_threads",c.hooks.size()},{"input_profile","Target-thread legacy and raw keyboard messages; buffered-only Raw Input is not observed"},{"raw_key_events",c.raw_events},{"legacy_key_events",c.legacy_events},
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
void HudDraw::begin(uint64_t swap,IDXGISwapChain* native,UINT flags,bool partial,bool isolated) noexcept {
    if(flags&DXGI_PRESENT_TEST)return;const auto error=GetLastError();
    try{
        auto& c=control();hud::Panel panel;
        {const auto progress=capture_progress();std::lock_guard lock(c.mutex);if(!c.connected){SetLastError(error);return;}progress_message(c,progress);if(c.settings.mode==hud::Mode::Hidden){SetLastError(error);return;}
            panel.mode=c.settings.mode;panel.fps=c.clocks[swap].value(now_ns());panel.swap=swap;
            const auto wide=[](const std::string& text){const auto length=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);std::wstring result(length,L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),result.data(),length);return result;};
            panel.mode_key=wide(hud::hotkey_name(c.settings.mode_key));panel.capture_key=wide(hud::hotkey_name(c.settings.capture_key));panel.message=wide(c.message.value(GetTickCount64()));}
        impl_=std::make_unique<Impl>();impl_->id=swap;
        NativeCall internal;
        DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(native->GetDesc(&desc)))throw std::runtime_error("HUD swap descriptor unavailable");
        if(!isolated && (desc.BufferCount!=1 || (desc.SwapEffect!=DXGI_SWAP_EFFECT_DISCARD && desc.SwapEffect!=DXGI_SWAP_EFFECT_SEQUENTIAL)))throw std::runtime_error("Multi-buffer HUD pixel restoration is unverified");
        Microsoft::WRL::ComPtr<ID3D11Device> device;if(FAILED(native->GetDevice(IID_PPV_ARGS(&device))))throw std::runtime_error("HUD requires a native DX11 device");
        if(!isolated && !(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED))throw std::runtime_error("HUD concurrency isolation requires isolated application storage in this profile");
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        Microsoft::WRL::ComPtr<IUnknown> identity;context.As(&identity);const auto record=find_native_record(identity.Get());
        if(!record)throw std::runtime_error("HUD immediate context is untracked");
        {std::lock_guard lock(record->mutex);if(!record->active_queries.empty())throw std::runtime_error("HUD skips active queries");}
        if(object_status().value("interface_gaps",0ull))throw std::runtime_error("HUD skips untracked interfaces");
        // The isolated renderer only copies private textures and the native
        // presentation surface. It never binds or reads application buffers;
        // outstanding CPU buffer mappings therefore remain untouched. Texture
        // and DXGI mappings are still excluded, as are unsynchronized paths.
        if(has_external_mapping(true,isolated))throw std::runtime_error("HUD skips active texture or external mappings");
        Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;BOOL value=FALSE;context->GetPredication(&predicate,&value);if(predicate)throw std::runtime_error("HUD skips active predication");
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_desc{};
        if(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapter_desc)))panel.adapter=adapter_desc.Description;
        const wchar_t* explicit_adapter=L"UNKNOWN";Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;DXGI_ADAPTER_DESC1 description1{};
        if(adapter && SUCCEEDED(adapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&description1)))explicit_adapter=(description1.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)?L"SW":L"HW";
        const auto level=device->GetFeatureLevel();panel.device=L"DX "+std::to_wstring((unsigned(level)>>12)&15)+L"."+std::to_wstring((unsigned(level)>>8)&15);
        Microsoft::WRL::ComPtr<IUnknown> device_identity;device.As(&device_identity);
        if(const auto record_device=find_native_record(device_identity.Get())){std::lock_guard lock(record_device->mutex);if(record_device->driver_type){const wchar_t* label=L"UNKNOWN";switch(*record_device->driver_type){case D3D_DRIVER_TYPE_UNKNOWN:label=explicit_adapter;break;case D3D_DRIVER_TYPE_HARDWARE:label=L"HW";break;case D3D_DRIVER_TYPE_WARP:label=L"WARP";break;case D3D_DRIVER_TYPE_REFERENCE:label=L"REF";break;case D3D_DRIVER_TYPE_SOFTWARE:label=L"SW";break;case D3D_DRIVER_TYPE_NULL:label=L"NULL";break;default:break;}panel.device+=L" ["+std::wstring(label)+L"]";}}
        impl_->pixels=std::make_unique<hud::PixelTransaction>();
        impl_->drawn=impl_->pixels->begin(native,panel,flags,partial,impl_->reason,isolated);
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
void HudDraw::discard_backup() noexcept {
    const auto error=GetLastError();if(impl_ && impl_->pixels){NativeCall internal;impl_->pixels->discard_backup();impl_->pixels.reset();}SetLastError(error);
}
}
