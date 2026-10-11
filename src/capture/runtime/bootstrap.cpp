// Native injection/transport milestone. Intentionally refuses frame requests
// until object tracking and complete command/resource serialization exist.
#include "protocol.hpp"
#include "swap_chain.hpp"
#include "factory.hpp"
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
    ++creation_depth;
    const HRESULT result=real_create(adapter,type,software,flags,levels,count,sdk,device,selected,context);
    const DWORD error=GetLastError();record_creation(result,selected);--creation_depth;SetLastError(error);return result;
}
HRESULT WINAPI create_swap(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE software,UINT flags,
    const D3D_FEATURE_LEVEL* levels,UINT count,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** swap,
    ID3D11Device** device,D3D_FEATURE_LEVEL* selected,ID3D11DeviceContext** context){
    ++creation_depth;
    const HRESULT result=real_create_swap(adapter,type,software,flags,levels,count,sdk,desc,swap,device,selected,context);
    const DWORD error=GetLastError();record_creation(result,selected);
    if(SUCCEEDED(result) && creation_depth==1)wrap_swap_chain(swap);
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
Json response(const Json& message){
    Json result={{"ok",false},{"version",protocol_version},{"request_id",message.value("request_id",std::string())},
                 {"pid",GetCurrentProcessId()},{"state",creations.load()?"Tracking":"Starting"},
                 {"device_creations",creations.load()},{"creation_failures",failures.load()},
                 {"feature_level",level.load()},{"hook_status",hook_status.load()},
                 {"capture_available",false},{"capability_gaps",{"COM object tracking","Present boundaries","resource snapshots","command serialization"}}};
    if(message.value("version",0u)!=protocol_version){result["error"]="protocol_version_mismatch";return result;}
    const auto command=message.value("command",std::string());
    if(command=="status"){
        result["ok"]=true;
        result["swap_chains"]=swap_chain_status();
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
    else if(command=="cancel"){result["ok"]=true;result["cancelled"]=false;}
    else if(command=="disconnect"){result["ok"]=true;result["state"]="Disconnected";}
    else if(command=="capture"){result["ok"]=false;result["error"]="capture_capability_incomplete";}
    else {result["ok"]=false;result["error"]="unknown_command";}
    return result;
}
DWORD WINAPI serve(void*){
    try{
        char session[64]{};DWORD n=GetEnvironmentVariableA("FLORAGPA_CAPTURE_SESSION",session,sizeof(session));
        if(!n || n>=sizeof(session))return ERROR_INVALID_PARAMETER;
        const auto name=pipe_name(session);Security security;
        Handle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,max_message,max_message,0,&security.attributes));
        if(pipe.value==INVALID_HANDLE_VALUE)return GetLastError();
        bool disconnected=false;
        while(!disconnected){
            Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)return GetLastError();
            OVERLAPPED ov{};ov.hEvent=event.value;
            if(!ConnectNamedPipe(pipe.value,&ov)){
                const auto error=GetLastError();
                if(error==ERROR_IO_PENDING){DWORD transferred=0;if(!GetOverlappedResult(pipe.value,&ov,&transferred,TRUE))return GetLastError();}
                else if(error!=ERROR_PIPE_CONNECTED)return error;
            }
            try{
                auto message=receive(pipe.value);auto answer=response(message);send(pipe.value,answer);
                disconnected=answer.value("ok",false) && message.value("command",std::string())=="disconnect";
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
