// Native startup injection and private capture control. Unsupported profiles
// remain explicit failures; only completed files cross into replay.
#include "protocol.hpp"
#include "swap_chain.hpp"
#include "factory.hpp"
#include "objects.hpp"
#include "capture_session.hpp"
#include "capture_requests.hpp"
#include "hud_control.hpp"
#include "capture_model.hpp"
#include <filesystem>
#include <d3d11.h>
#include <detours.h>
#include <sddl.h>
#include <psapi.h>
#include <atomic>
#include <vector>
#include <fstream>
#include <objbase.h>
#include "auto_detect.hpp"

using namespace flora::capture;
namespace {
std::atomic<unsigned> creations{0};
std::atomic<unsigned> failures{0};
std::atomic<unsigned> level{0};
std::atomic<long> hook_status{ERROR_NOT_READY};
thread_local unsigned creation_depth=0;
decltype(&D3D11CreateDevice) real_create=D3D11CreateDevice;
decltype(&D3D11CreateDeviceAndSwapChain) real_create_swap=D3D11CreateDeviceAndSwapChain;
decltype(&GetRawInputBuffer) real_raw_buffer=GetRawInputBuffer;
UINT WINAPI raw_buffer(PRAWINPUT data,PUINT bytes,UINT header_size){
    const auto result=real_raw_buffer(data,bytes,header_size);const auto error=GetLastError();
    // Only inspect successful native output. Query/error calls need no buffer
    // access, and the application still owns all records and cleanup.
    if(data && bytes && result && result!=UINT(-1) && header_size==sizeof(RAWINPUTHEADER))hud_raw_buffer(data,*bytes,result);
    SetLastError(error);return result;
}
void record_creation(HRESULT hr,D3D_FEATURE_LEVEL* selected){
    if(creation_depth==1){
        if(SUCCEEDED(hr)){++creations;if(selected)level=static_cast<unsigned>(*selected);}
        else ++failures;
    }
}
void record_driver(ID3D11Device** device,D3D_DRIVER_TYPE type) noexcept {
    try{if(device && *device)if(auto record=object_record(*device)){std::lock_guard lock(record->mutex);record->driver_type=type;}}catch(...){}
}
HRESULT WINAPI create_device(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,
    const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* selected,
    ID3D11DeviceContext** context){
    if(native_call_depth)return real_create(adapter,type,software,flags,levels,count,sdk,device,selected,context);
    ++creation_depth; NativeCall internal;
    const HRESULT result=real_create(unwrap(adapter),type,software,flags,levels,count,sdk,device,selected,context);
    const DWORD error=GetLastError();record_creation(result,selected);
    if(SUCCEEDED(result)){if(!selected && device && *device)level=(*device)->GetFeatureLevel();wrap_object(device,"D3D11CreateDevice");wrap_object(context,"D3D11CreateDevice");record_driver(device,type);}
    --creation_depth;SetLastError(error);return result;
}
HRESULT WINAPI create_swap(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,
    const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** swap,
    ID3D11Device** device,D3D_FEATURE_LEVEL* selected,ID3D11DeviceContext** context){
    if(native_call_depth)return real_create_swap(adapter,type,software,flags,levels,count,sdk,desc,swap,device,selected,context);
    ++creation_depth; NativeCall internal;
    const HRESULT result=real_create_swap(unwrap(adapter),type,software,flags,levels,count,sdk,desc,swap,device,selected,context);
    const DWORD error=GetLastError();record_creation(result,selected);
    if(SUCCEEDED(result) && creation_depth==1){
        if(!selected && device && *device)level=(*device)->GetFeatureLevel();
        wrap_swap_chain(swap);wrap_object(device,"D3D11CreateDeviceAndSwapChain");wrap_object(context,"D3D11CreateDeviceAndSwapChain");
        record_driver(device,type);
    }
    --creation_depth;SetLastError(error);return result;
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    Security(){
        HANDLE raw=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))win_error("Read session user");
        Handle token(raw);DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
        std::vector<std::uint8_t> data(size);
        if(!GetTokenInformation(token.value,TokenUser,data.data(),size,&size))win_error("Read session SID");
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid))win_error("Encode session SID");
        std::wstring sddl;
        try{sddl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";}catch(...){LocalFree(sid);throw;}
        LocalFree(sid);
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))win_error("Create private session ACL");
        attributes.lpSecurityDescriptor=descriptor;
    }
    ~Security(){if(descriptor)LocalFree(descriptor);}
};
std::string service_session;
std::filesystem::path capture_directory;
bool automatic_session=false;
std::atomic<bool> automatic_initialized{false};
Json response(const Json& message,ULONG client_pid){
    Json result={{"ok",false},{"version",protocol_version},{"request_id",message.value("request_id",std::string())},
                 {"session_id",service_session},{"pid",GetCurrentProcessId()}};
#ifdef FLORA_CAPTURE_FINAL_MSAA_DIAGNOSTICS
    result["development_runtime"]="final_msaa_readbacks";
#endif
    const auto number_matches=[&](const char* key,uint64_t value){return message.contains(key) && message[key].is_number_unsigned() && message[key].get<uint64_t>()==value;};
    if(!number_matches("version",protocol_version)){result["error"]="protocol_version_mismatch";return result;}
    if(message.value("session_id",std::string())!=service_session || !number_matches("target_pid",GetCurrentProcessId()) || !number_matches("client_pid",client_pid)){result["error"]="session_process_mismatch";return result;}
    if(message.contains("swap_chain") && (!message["swap_chain"].is_number_unsigned() || (message["swap_chain"].get<uint64_t>()==0 && message.value("command",std::string())!="select"))){result["error"]="invalid_swap_chain";return result;}
    // Validate the UUID before using it as a file name. Paths never come from IPC.
    pipe_name(message.at("request_id").get<std::string>());
    const auto command=message.value("command",std::string());
    if(message.contains("resource_limit_mib") && (command!="capture" || !message["resource_limit_mib"].is_number_unsigned() ||
       !message["resource_limit_mib"].get<uint64_t>() || message["resource_limit_mib"].get<uint64_t>()>max_resource_limit_mib)){
        result["error"]="invalid_resource_limit";return result;
    }
    const auto swaps=swap_chain_status();const auto objects=object_status();
    bool available=capture_request_available(swaps,objects);
    result["ok"]=true;
    if(command=="capture"){
        const auto armed=submit_capture_request(message.value("swap_chain",0ull),message.at("request_id").get<std::string>(),message.value("resource_limit_mib",default_resource_limit_mib));
        result["ok"]=armed.value("ok",false);if(armed.contains("error"))result["error"]=armed["error"];
        hud_feedback(armed);
    }else if(command=="select"){
        const auto selected=select_capture_swap(message.at("swap_chain").get<uint64_t>());result["ok"]=selected.value("ok",false);if(selected.contains("error"))result["error"]=selected["error"];
    }else if(command=="cancel"){
        const auto cancelled=cancel_capture();result["cancelled"]=cancelled.value("cancelled",false);
    }else if(command=="disconnect"){
        disconnect_capture_requests();available=false;
        disconnect_hud();
    }else if(command!="status"){result["ok"]=false;result["error"]="unknown_command";}
    const auto progress=capture_progress();const auto phase=progress.value("state",std::string("Tracking"));
    result["state"]=!capture_requests_connected()?"Disconnected":progress.value("busy",false)?phase:creations.load()?"Tracking":"Starting";
    result["capture_progress"]=progress;
    result["hud"]=hud_status();
    if(phase=="Completed" || phase=="Cancelled" || phase=="Failed"){
        result["last_capture"]=progress;
        if(progress.contains("file"))result["last_capture"]["path"]=progress["file"];
    }
    result["capture_available"]=available;result["swap_chains"]=swaps;result["objects"]=objects;
    result["capture_limits"]={{"default_resource_limit_mib",default_resource_limit_mib},{"max_resource_limit_mib",max_resource_limit_mib}};
    result["device_creations"]=creations.load();result["creation_failures"]=failures.load();result["feature_level"]=level.load();result["hook_status"]=hook_status.load();
    result["attachment"]=automatic_session?"auto_detect":"launch";
    result["capability_gaps"]={"Single-sample RGBA8/BGRA8/RGB10A2; dirty/scroll and flags other than ALLOW_TEARING unsupported","Query, empty-box/flagged/planar Updates, region/format copies, planar Maps and hidden states are incomplete","Deferred nesting beyond bounded journal limits, unverified Map baselines/layouts and hidden states unsupported","Deferred CS/OM initial KEEP requires a retained native UAV and successful execution-time readback; numeric UINT_MAX remains unsupported","Single immediate-context execution thread only"};
    if(command=="status"){
        result["factories"]=factory_status();
        HMODULE modules[1024]{};DWORD needed=0;
        if(K32EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&needed) && needed<=sizeof(modules)){
            result["loaded_modules"]=Json::array();
            for(DWORD i=0;i<needed/sizeof(HMODULE);++i){
                wchar_t path[32768]{};DWORD length=GetModuleFileNameW(modules[i],path,32768);
                if(!length || length>=32768)continue;
                int bytes=WideCharToMultiByte(CP_UTF8,0,path,static_cast<int>(length),nullptr,0,nullptr,nullptr);
                std::string value(bytes,'\0');WideCharToMultiByte(CP_UTF8,0,path,static_cast<int>(length),value.data(),bytes,nullptr,nullptr);
                result["loaded_modules"].push_back(value);
            }
        }else{result["ok"]=false;result["error"]="module_inventory_failed";}
    }
    return result;
}
DWORD WINAPI serve(void*){
    try{
      if(!automatic_session){
        char session[64]{};DWORD n=GetEnvironmentVariableA("FLORAGPA_CAPTURE_SESSION",session,sizeof(session));
        if(!n || n>=sizeof(session))return ERROR_INVALID_PARAMETER;
        service_session=session;
        wchar_t output[32768]{};const auto count=GetEnvironmentVariableW(L"FLORAGPA_CAPTURE_OUTPUT",output,32768);
        if(!count || count>=32768)return ERROR_INVALID_PARAMETER;
        capture_directory=output;if(!capture_directory.is_absolute() || !std::filesystem::is_directory(capture_directory))return ERROR_INVALID_PARAMETER;
        configure_capture_requests(capture_directory);
        auto setting=[](const char* key,const char* fallback){char value[128]{};const auto size=GetEnvironmentVariableA(key,value,sizeof(value));if(size>=sizeof(value))throw std::invalid_argument("HUD setting is too long");return std::string(size?value:fallback);};
        hud::Settings hud_settings;
        hud_settings.mode=hud::parse_mode(setting("FLORAGPA_CAPTURE_HUD","compact"));
        hud_settings.mode_key=hud::parse_hotkey(setting("FLORAGPA_CAPTURE_HUD_HOTKEY","Ctrl+F1"));
        hud_settings.capture_key=hud::parse_hotkey(setting("FLORAGPA_CAPTURE_CAPTURE_HOTKEY","Ctrl+Shift+C"));
        configure_hud(hud_settings);
      }
        const auto name=pipe_name(service_session);Security security;
        Handle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,max_message,max_message,0,&security.attributes));
        if(pipe.value==INVALID_HANDLE_VALUE)return GetLastError();
        for(;;){
            Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return GetLastError();
            OVERLAPPED ov{};ov.hEvent=event.value;
            if(!ConnectNamedPipe(pipe.value,&ov)){
                const auto error=GetLastError();
                if(error==ERROR_IO_PENDING){DWORD transferred=0;if(!GetOverlappedResult(pipe.value,&ov,&transferred,TRUE))return GetLastError();}
                else if(error!=ERROR_PIPE_CONNECTED)return error;
            }
            try{
                ULONG client_pid=0;if(!GetNamedPipeClientProcessId(pipe.value,&client_pid) || !client_pid)throw std::runtime_error("Cannot identify control client");
                auto message=receive(pipe.value);auto answer=response(message,client_pid);send(pipe.value,answer);
                // Client acknowledges consumption, avoiding synchronous FlushFileBuffers.
                auto ack=receive(pipe.value);
                if(ack.value("request_id",std::string())!=answer.value("request_id",std::string()))throw std::runtime_error("Ack mismatch");
            }catch(...){/* A bad client must not throw through target code. */}
            DisconnectNamedPipe(pipe.value);
        }
        return 0;
    }catch(...){return ERROR_INVALID_DATA;}
}
}

extern "C" __declspec(dllexport) DWORD WINAPI FloraCaptureInitialize(DWORD version,
    const autodetect::Lease* lease,const autodetect::Entries* original,autodetect::Entries* hooks) {
    if(version!=autodetect::abi_version || !lease || !original || !hooks ||
       !original->device || !original->swap || !original->factory || !original->factory1 || !original->factory2)
        return ERROR_INVALID_PARAMETER;
    if(automatic_initialized.exchange(true))return ERROR_ALREADY_INITIALIZED;
    try {
        automatic_session=true;
        GUID guid{};if(FAILED(CoCreateGuid(&guid)))return ERROR_GEN_FAILURE;
        wchar_t uuid[40]{};StringFromGUID2(guid,uuid,40);
        service_session.clear();for(unsigned i=1;i<37;++i)service_session+=static_cast<char>(towlower(uuid[i]));
        capture_directory=std::filesystem::path(lease->output)/std::filesystem::path(service_session);
        if(!capture_directory.is_absolute() || !std::filesystem::create_directory(capture_directory))return ERROR_PATH_NOT_FOUND;
        configure_capture_requests(capture_directory);
        hud::Settings settings;
        settings.mode=hud::parse_mode(lease->hud);settings.mode_key=hud::parse_hotkey(lease->hud_key);
        settings.capture_key=hud::parse_hotkey(lease->capture_key);settings.validate();configure_hud(settings);
        real_create=original->device;real_create_swap=original->swap;
        hooks->device=create_device;hooks->swap=create_swap;configure_factory_entries(*original,*hooks);
        LONG error=DetourTransactionBegin();
        if(error==NO_ERROR)error=DetourUpdateThread(GetCurrentThread());
        if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&real_raw_buffer),raw_buffer);
        if(error==NO_ERROR)error=DetourTransactionCommit();else DetourTransactionAbort();
        hook_status=error;if(error!=NO_ERROR)return static_cast<DWORD>(error);
        HANDLE thread=CreateThread(nullptr,0,serve,nullptr,0,nullptr);
        if(!thread)return GetLastError();CloseHandle(thread);
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        const auto utf8=[](const std::filesystem::path& p){const auto bytes=p.u8string();return std::string(bytes.begin(),bytes.end());};
        const auto file=capture_directory/L"session.json";
        Json manifest={{"version",protocol_version},{"session_id",service_session},{"pid",GetCurrentProcessId()},
            {"process_created",autodetect::created(GetCurrentProcess())},{"attachment","auto_detect"},
            {"executable",utf8(executable)},{"runtime",utf8(lease->runtime)},
            {"output_directory",utf8(capture_directory)},{"session_file",utf8(file)}};
        {std::ofstream stream(capture_directory/L"session.tmp",std::ios::binary);stream<<manifest.dump(2);stream.close();if(!stream)return ERROR_WRITE_FAULT;}
        std::filesystem::rename(capture_directory/L"session.tmp",file);
        return ERROR_SUCCESS;
    }catch(...){return ERROR_INVALID_DATA;}
}

BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){
    if(DetourIsHelperProcess())return TRUE;
    if(reason==DLL_PROCESS_ATTACH){
        // Auto-Detect enters through the explicit ABI outside the loader lock.
        // The existing suspended-launch path retains its early hook bootstrap.
        char session[64]{};
        if(!GetEnvironmentVariableA("FLORAGPA_CAPTURE_SESSION",session,sizeof(session)))return TRUE;
        // An inherited session variable is not proof of suspended injection.
        // Auto-Detect can load us in a child of an old injected launcher.
        if(!DetourRestoreAfterWith())return TRUE;
        // Static CRT and thread_local recursion guard require thread callbacks.
        (void)module;
        LONG error=DetourTransactionBegin();
        if(error==NO_ERROR)error=DetourUpdateThread(GetCurrentThread());
        if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&real_create),create_device);
        if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&real_create_swap),create_swap);
        if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&real_raw_buffer),raw_buffer);
        if(error==NO_ERROR)error=attach_factory_hooks();
        if(error!=NO_ERROR){DetourTransactionAbort();hook_status=error;return FALSE;}
        hook_status=DetourTransactionCommit();if(hook_status!=NO_ERROR)return FALSE;
        HANDLE thread=CreateThread(nullptr,0,serve,nullptr,0,nullptr);
        // Committed hooks point into this module: never return FALSE and let
        // the loader unmap it after hook installation. Handshake will report
        // service startup failure while the application keeps forwarding.
        if(!thread){hook_status=GetLastError();return TRUE;}CloseHandle(thread);
    }
    // Process teardown only. Never detach hooks under active target threads.
    return TRUE;
}
