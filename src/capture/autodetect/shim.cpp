// AppInit bootstrap: no Qt, D3D import library, JSON, filesystem or IPC startup.
#include "auto_detect.hpp"
#include <detours.h>
#include <sddl.h>
#include "detour_threads.hpp"

using namespace flora::capture::autodetect;
namespace {
const Lease* lease=nullptr;
HANDLE mapping=nullptr, owner=nullptr, broker=nullptr;
HANDLE progress_mapping=nullptr;
Progress* progress=nullptr;
AttachProgress* attachment=nullptr;
thread_local bool loader_phase=false;
void acknowledge(AttachStage stage,DWORD error=0) {
    if(!attachment)return;
    attachment->error=error;
    InterlockedExchange(&attachment->stage,static_cast<LONG>(stage));
}
void open_progress() {
    if(!lease->target[0])return;
    wchar_t path[32768]{};
    if(!GetModuleFileNameW(nullptr,path,32768) || _wcsicmp(path,lease->target))return;
    progress_mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,progress_mapping_name(lease->started).c_str());
    if(!progress_mapping)return;
    progress=static_cast<Progress*>(MapViewOfFile(progress_mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Progress)));
    if(!progress || progress->version!=abi_version || progress->epoch!=lease->started)return;
    attachment=claim_progress(*progress,GetCurrentProcessId(),created(GetCurrentProcess()));
}
std::uint64_t lease_epoch=0;
Entries native{}, dispatch{};
INIT_ONCE initialized=INIT_ONCE_STATIC_INIT;
SRWLOCK patch_lock=SRWLOCK_INIT;
thread_local bool loading=false,patching=false;
decltype(&LoadLibraryW) load_w=LoadLibraryW;
decltype(&LoadLibraryA) load_a=LoadLibraryA;
decltype(&LoadLibraryExW) load_exw=LoadLibraryExW;
decltype(&LoadLibraryExA) load_exa=LoadLibraryExA;
bool live() noexcept {
    return lease && lease->enabled && lease->started==lease_epoch && WaitForSingleObject(owner,0)==WAIT_TIMEOUT &&
        WaitForSingleObject(broker,0)==WAIT_TIMEOUT;
}
bool attach_graphics();
BOOL CALLBACK initialize(PINIT_ONCE,PVOID,PVOID*) {
    if(!live())return TRUE;
    loading=true;
    // Called from a graphics entry, after the loader has returned to the app.
    HMODULE runtime=load_exw(lease->runtime,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    // A DXGI factory may precede loading D3D11. Runtime imports now make both
    // modules available; finish the entry trampolines before publishing them.
    const bool patched=runtime && attach_graphics();
    auto entry=runtime?reinterpret_cast<Initialize>(GetProcAddress(runtime,"FloraCaptureInitialize")):nullptr;
    Entries hooks{};
    const DWORD result=entry && patched?entry(abi_version,lease,&native,&hooks):
        (runtime?(patched?ERROR_PROC_NOT_FOUND:ERROR_INVALID_FUNCTION):GetLastError());
    if(result==ERROR_SUCCESS){dispatch=hooks;acknowledge(AttachStage::runtime_ready);}
    else {
        acknowledge(AttachStage::failed,result);
        wchar_t message[128]{};swprintf_s(message,L"FloraGPA Auto-Detect initialization failed: %lu\n",result);
        OutputDebugStringW(message);
        // Graphics-entry phase only: never create files from DllMain.
        Failure failure;failure.pid=GetCurrentProcessId();failure.error=result;
        failure.process_created=created(GetCurrentProcess());
        GetModuleFileNameW(nullptr,failure.executable,1024);
        wchar_t path[4352]{};swprintf_s(path,L"%s\\%lu-%llu.failure",lease->output,failure.pid,failure.process_created);
        HANDLE file=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file!=INVALID_HANDLE_VALUE){DWORD written=0;WriteFile(file,&failure,sizeof(failure),&written,nullptr);CloseHandle(file);}
    }
    // Once loaded, runtime may own raw-input hooks: do not unload on failure.
    loading=false;return TRUE;
}
void ready() {
    const auto error=GetLastError();
    // Always join an in-flight initialization, even if the lease was just
    // disabled. This also publishes dispatch to other calling threads.
    if(!loading)InitOnceExecuteOnce(&initialized,initialize,nullptr,nullptr);
    SetLastError(error);
}
HRESULT WINAPI device(IDXGIAdapter* a,D3D_DRIVER_TYPE t,HMODULE s,UINT f,const D3D_FEATURE_LEVEL* l,UINT n,UINT v,
    ID3D11Device** d,D3D_FEATURE_LEVEL* chosen,ID3D11DeviceContext** c) {
    ready();return (dispatch.device?dispatch.device:native.device)(a,t,s,f,l,n,v,d,chosen,c);
}
HRESULT WINAPI swap(IDXGIAdapter* a,D3D_DRIVER_TYPE t,HMODULE s,UINT f,const D3D_FEATURE_LEVEL* l,UINT n,UINT v,
    const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** out,ID3D11Device** d,D3D_FEATURE_LEVEL* chosen,ID3D11DeviceContext** c) {
    ready();return (dispatch.swap?dispatch.swap:native.swap)(a,t,s,f,l,n,v,desc,out,d,chosen,c);
}
HRESULT WINAPI factory(REFIID iid,void** out){ready();return (dispatch.factory?dispatch.factory:native.factory)(iid,out);}
HRESULT WINAPI factory1(REFIID iid,void** out){ready();return (dispatch.factory1?dispatch.factory1:native.factory1)(iid,out);}
HRESULT WINAPI factory2(UINT f,REFIID iid,void** out){ready();return (dispatch.factory2?dispatch.factory2:native.factory2)(f,iid,out);}
bool attach_graphics() {
    if(patching)return true;
    patching=true;
    // Resolve under the OS loader lock before taking our patch lock. A different
    // LoadLibrary caller can still be executing a DLL initializer.
    auto d3d=GetModuleHandleW(L"d3d11.dll"),dxgi=GetModuleHandleW(L"dxgi.dll");
    const auto find=[](HMODULE m,const char* name){return m?GetProcAddress(m,name):nullptr;};
    const FARPROC addresses[]={find(d3d,"D3D11CreateDevice"),find(d3d,"D3D11CreateDeviceAndSwapChain"),
        find(dxgi,"CreateDXGIFactory"),find(dxgi,"CreateDXGIFactory1"),find(dxgi,"CreateDXGIFactory2")};
    AcquireSRWLockExclusive(&patch_lock);
    const bool needed=(addresses[0] && !native.device) || (addresses[1] && !native.swap) ||
        (addresses[2] && !native.factory) || (addresses[3] && !native.factory1) || (addresses[4] && !native.factory2);
    if(!needed){ReleaseSRWLockExclusive(&patch_lock);patching=false;return true;}
    flora::capture::DetourThreads threads(!loader_phase);
    Entries before=native;
    LONG error=DetourTransactionBegin();
    if(error==NO_ERROR)error=DetourUpdateThread(GetCurrentThread());
    const auto hook=[&](auto& entry,FARPROC address,auto replacement){
        if(entry || !address || error!=NO_ERROR)return;
        entry=reinterpret_cast<decltype(replacement)>(address);
        if(entry)error=DetourAttach(reinterpret_cast<PVOID*>(&entry),reinterpret_cast<PVOID>(replacement));
    };
    hook(native.device,addresses[0],device);hook(native.swap,addresses[1],swap);
    hook(native.factory,addresses[2],factory);hook(native.factory1,addresses[3],factory1);
    hook(native.factory2,addresses[4],factory2);
    if(error==NO_ERROR)error=threads.enlist();
    if(error==NO_ERROR)error=DetourTransactionCommit();else DetourTransactionAbort();
    if(error!=NO_ERROR)native=before;
    ReleaseSRWLockExclusive(&patch_lock);patching=false;return error==NO_ERROR;
}
HMODULE WINAPI hooked_w(LPCWSTR path){auto m=load_w(path);const auto e=GetLastError();if(m)attach_graphics();SetLastError(e);return m;}
HMODULE WINAPI hooked_a(LPCSTR path){auto m=load_a(path);const auto e=GetLastError();if(m)attach_graphics();SetLastError(e);return m;}
HMODULE WINAPI hooked_exw(LPCWSTR path,HANDLE h,DWORD f){auto m=load_exw(path,h,f);const auto e=GetLastError();if(m)attach_graphics();SetLastError(e);return m;}
HMODULE WINAPI hooked_exa(LPCSTR path,HANDLE h,DWORD f){auto m=load_exa(path,h,f);const auto e=GetLastError();if(m)attach_graphics();SetLastError(e);return m;}
void release_lease() {
    if(owner)CloseHandle(owner);if(broker)CloseHandle(broker);
    if(lease)UnmapViewOfFile(lease);if(mapping)CloseHandle(mapping);
    owner=nullptr;broker=nullptr;lease=nullptr;mapping=nullptr;
}
bool eligible() {
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768) || excluded(path))return false;
    // Launchers may retain an old explicit-launch environment indefinitely.
    // Only an actually loaded runtime owns the entry hooks in this process.
    if(GetModuleHandleW(L"FloraCaptureRuntime.dll"))return false;
    mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,mapping_name);if(!mapping)return false;
    lease=static_cast<const Lease*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(Lease)));
    DWORD session=0;
    if(!lease || lease->version!=abi_version || !lease->enabled ||
       !ProcessIdToSessionId(GetCurrentProcessId(),&session) || session!=lease->desktop_session ||
       created(GetCurrentProcess())<lease->started)return false;
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    BYTE data[512]{};DWORD size=0;
    const bool got=GetTokenInformation(token,TokenUser,data,sizeof(data),&size)!=FALSE;CloseHandle(token);
    LPWSTR sid=nullptr;if(!got || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data)->User.Sid,&sid))return false;
    const bool same=wcscmp(sid,lease->sid)==0;LocalFree(sid);if(!same)return false;
    lease_epoch=lease->started;
    owner=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,lease->owner_pid);
    broker=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,lease->broker_pid);
    return owner && broker && created(owner)==lease->owner_created && created(broker)==lease->broker_created && live();
}
DWORD WINAPI install(void*) {
    if(!live())return ERROR_CANCELLED;
    acknowledge(AttachStage::installing);
    flora::capture::DetourThreads threads(!loader_phase);
    auto error=DetourTransactionBegin();
    if(error==NO_ERROR)error=DetourUpdateThread(GetCurrentThread());
    if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&load_w),hooked_w);
    if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&load_a),hooked_a);
    if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&load_exw),hooked_exw);
    if(error==NO_ERROR)error=DetourAttach(reinterpret_cast<PVOID*>(&load_exa),hooked_exa);
    if(error==NO_ERROR)error=threads.enlist();
    if(error==NO_ERROR)error=DetourTransactionCommit();else DetourTransactionAbort();
    if(error!=NO_ERROR){acknowledge(AttachStage::failed,error);return error;}
    if(!attach_graphics()){acknowledge(AttachStage::failed,ERROR_INVALID_FUNCTION);return ERROR_INVALID_FUNCTION;}
    if(attachment)InterlockedCompareExchange(&attachment->stage,static_cast<LONG>(AttachStage::hooks_ready),static_cast<LONG>(AttachStage::installing));
    return ERROR_SUCCESS;
}
}
BOOL WINAPI DllMain(HINSTANCE,DWORD reason,LPVOID) {
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    if(!eligible()){release_lease();return TRUE;}
    open_progress();
    // A broker-requested load happens in an already running process. Install
    // outside the loader lock so thread enlistment cannot deadlock DLL startup.
    if(attachment && InterlockedCompareExchange(&attachment->stage,0,0)==static_cast<LONG>(AttachStage::requested)){
        HANDLE thread=CreateThread(nullptr,0,install,nullptr,0,nullptr);
        if(thread)CloseHandle(thread);else acknowledge(AttachStage::failed,GetLastError());
    }else{loader_phase=true;install(nullptr);loader_phase=false;}
    return TRUE;
}
