// Native startup injection and private capture control. Unsupported profiles
// remain explicit failures; only completed files cross into replay.
#include "protocol.hpp"
#include "swap_chain.hpp"
#include "factory.hpp"
#include "objects.hpp"
#include "capture_session.hpp"
#include "capture_model.hpp"
#include <filesystem>
#include <d3d11.h>
#include <detours.h>
#include <sddl.h>
#include <psapi.h>
#include <atomic>
#include <vector>

using namespace flora::capture;
namespace {
std::atomic<unsigned> creations{0};
std::atomic<unsigned> failures{0};
std::atomic<unsigned> level{0};
std::atomic<long> hook_status{ERROR_NOT_READY};
thread_local unsigned creation_depth=0;
decltype(&D3D11CreateDevice) real_create=D3D11CreateDevice;
decltype(&D3D11CreateDeviceAndSwapChain) real_create_swap=D3D11CreateDeviceAndSwapChain;
void record_creation(HRESULT hr,D3D_FEATURE_LEVEL* selected){
    if(creation_depth==1){
        if(SUCCEEDED(hr)){++creations;if(selected)level=static_cast<unsigned>(*selected);}
        else ++failures;
    }
}
HRESULT WINAPI create_device(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,
    const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,ID3D11Device** device,D3D_FEATURE_LEVEL* selected,
    ID3D11DeviceContext** context){
    if(native_call_depth)return real_create(adapter,type,software,flags,levels,count,sdk,device,selected,context);
    ++creation_depth; NativeCall internal;
    const HRESULT result=real_create(unwrap(adapter),type,software,flags,levels,count,sdk,device,selected,context);
    const DWORD error=GetLastError();record_creation(result,selected);
    if(SUCCEEDED(result)){if(!selected && device && *device)level=(*device)->GetFeatureLevel();wrap_object(device,"D3D11CreateDevice");wrap_object(context,"D3D11CreateDevice");}
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
bool service_disconnected=false;
bool eligible(const Json& row){
    return row.value("recent_present",false) && row.value("successful_presents",0ull)>=2 &&
        supported_swap_buffers(row.value("swap_effect",99u),row.value("buffer_count",0u)) && row.value("samples",0u)==1 &&
        (row.value("format",0u)==DXGI_FORMAT_R8G8B8A8_UNORM || row.value("format",0u)==DXGI_FORMAT_B8G8R8A8_UNORM);
}
Json response(const Json& message,ULONG client_pid){
    Json result={{"ok",false},{"version",protocol_version},{"request_id",message.value("request_id",std::string())},
                 {"session_id",service_session},{"pid",GetCurrentProcessId()}};
    const auto number_matches=[&](const char* key,uint64_t value){return message.contains(key) && message[key].is_number_unsigned() && message[key].get<uint64_t>()==value;};
    if(!number_matches("version",protocol_version)){result["error"]="protocol_version_mismatch";return result;}
    if(message.value("session_id",std::string())!=service_session || !number_matches("target_pid",GetCurrentProcessId()) || !number_matches("client_pid",client_pid)){result["error"]="session_process_mismatch";return result;}
    if(message.contains("swap_chain") && (!message["swap_chain"].is_number_unsigned() || message["swap_chain"].get<uint64_t>()==0)){result["error"]="invalid_swap_chain";return result;}
    // Validate the UUID before using it as a file name. Paths never come from IPC.
    pipe_name(message.at("request_id").get<std::string>());
    const auto command=message.value("command",std::string());
    const auto swaps=swap_chain_status();const auto objects=object_status();
    bool available=false;
    for(const auto& row:swaps["active"])available=available || eligible(row);
    available=available && objects.value("interface_gaps",0ull)==0 && swaps.value("tracking_failures",0u)==0 && !service_disconnected;
    result["ok"]=true;
    if(command=="capture"){
        if(service_disconnected){result["ok"]=false;result["error"]="disconnected";}
        else if(capture_progress().value("busy",false)){result["ok"]=false;result["error"]="busy";}
        else if(!available){result["ok"]=false;result["error"]="capture_capability_incomplete";}
        else{
            uint64_t selected=message.value("swap_chain",0ull);std::vector<uint64_t> candidates;
            for(const auto& row:swaps["active"])if(eligible(row) && (selected?row["id"]==selected:row.value("foreground",false)))candidates.push_back(row["id"].get<uint64_t>());
            if(candidates.size()!=1){result["ok"]=false;result["error"]=selected?"swap_chain_unavailable":"swap_chain_selection_required";}
            else{
                const auto name="frame-"+message.at("request_id").get<std::string>()+".gpa_frame";
                const auto armed=arm_capture(candidates.front(),capture_directory/name);
                result["ok"]=armed.value("ok",false);if(armed.contains("error"))result["error"]=armed["error"];
            }
        }
    }else if(command=="cancel"){
        const auto cancelled=cancel_capture();result["cancelled"]=cancelled.value("cancelled",false);
    }else if(command=="disconnect"){
        disconnect_capture();service_disconnected=true;available=false;
    }else if(command!="status"){result["ok"]=false;result["error"]="unknown_command";}
    const auto progress=capture_progress();const auto phase=progress.value("state",std::string("Tracking"));
    result["state"]=service_disconnected?"Disconnected":progress.value("busy",false)?phase:creations.load()?"Tracking":"Starting";
    result["capture_progress"]=progress;
    if(phase=="Completed" || phase=="Cancelled" || phase=="Failed"){
        result["last_capture"]=progress;
        if(progress.contains("file"))result["last_capture"]["path"]=progress["file"];
    }
    result["capture_available"]=available;result["swap_chains"]=swaps;result["objects"]=objects;
    result["device_creations"]=creations.load();result["creation_failures"]=failures.load();result["feature_level"]=level.load();result["hook_status"]=hook_status.load();
    result["capability_gaps"]={"Single-sample RGBA8/BGRA8; dirty/scroll and flags other than ALLOW_TEARING unsupported","Query, boxed/flagged/planar Updates, region/format copies, deferred contexts, planar Maps and hidden states are incomplete","Single immediate-context application thread only"};
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
        char session[64]{};DWORD n=GetEnvironmentVariableA("FLORAGPA_CAPTURE_SESSION",session,sizeof(session));
        if(!n || n>=sizeof(session))return ERROR_INVALID_PARAMETER;
        service_session=session;
        wchar_t output[32768]{};const auto count=GetEnvironmentVariableW(L"FLORAGPA_CAPTURE_OUTPUT",output,32768);
        if(!count || count>=32768)return ERROR_INVALID_PARAMETER;
        capture_directory=output;if(!capture_directory.is_absolute() || !std::filesystem::is_directory(capture_directory))return ERROR_INVALID_PARAMETER;
        const auto name=pipe_name(session);Security security;
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

BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){
    if(DetourIsHelperProcess())return TRUE;
    if(reason==DLL_PROCESS_ATTACH){
        if(!DetourRestoreAfterWith())return FALSE;
        // Static CRT and thread_local recursion guard require thread callbacks.
        (void)module;
        LONG error=DetourTransactionBegin();
        if(error==NO_ERROR)error=DetourUpdateThread(GetCurrentThread());
        if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&real_create),create_device);
        if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&real_create_swap),create_swap);
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
