#include "active_attach.hpp"
#include "attach_policy.hpp"
#include <tlhelp32.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>

namespace flora::capture::autodetect {
fs::path validate_target(const std::wstring& path) {
    if(!fs::is_regular_file(path))throw std::invalid_argument("Target executable does not exist");
    const auto result=fs::canonical(path);
    std::ifstream stream(result,std::ios::binary);
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    stream.read(reinterpret_cast<char*>(&dos),sizeof(dos));
    if(!stream || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>16*1024*1024)
        throw std::invalid_argument("Target is not a valid executable");
    stream.seekg(dos.e_lfanew);stream.read(reinterpret_cast<char*>(&nt),sizeof(nt));
    if(!stream || nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || (nt.FileHeader.Characteristics&IMAGE_FILE_DLL))
        throw std::invalid_argument("Auto-Detect target must be an x64 executable");
    if(result.wstring().size()>=1024)throw std::invalid_argument("Auto-Detect target path is too long");
    if(excluded(result.c_str()))throw std::invalid_argument("Auto-Detect excludes this application");
    return result;
}
namespace {
std::wstring image_path(HANDLE process) {
    wchar_t value[32768]{};DWORD size=32768;
    return QueryFullProcessImageNameW(process,0,value,&size)?std::wstring(value,size):std::wstring();
}
bool native_x64(HANDLE process) {
    USHORT process_machine=0,native_machine=0;
    return IsWow64Process2(process,&process_machine,&native_machine) &&
        process_machine==IMAGE_FILE_MACHINE_UNKNOWN && native_machine==IMAGE_FILE_MACHINE_AMD64;
}
// Resolve the containing system module in the target; never assume that a local
// function pointer has the same address in another process (forwarded exports).
LPTHREAD_START_ROUTINE remote_loader(DWORD pid,bool& already_attached) {
    const auto entry=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HMODULE containing=nullptr;
    if(!entry || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(entry),&containing))return nullptr;
    wchar_t path[32768]{};if(!GetModuleFileNameW(containing,path,32768))return nullptr;
    const auto offset=reinterpret_cast<ULONG_PTR>(entry)-reinterpret_cast<ULONG_PTR>(containing);
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid));
    if(snapshot.value==INVALID_HANDLE_VALUE)return nullptr;
    MODULEENTRY32W module{sizeof(module)};
    LPTHREAD_START_ROUTINE loader=nullptr;
    if(Module32FirstW(snapshot.value,&module))do {
        if(_wcsicmp(module.szModule,L"FloraCaptureRuntime.dll")==0)already_attached=true;
        if(same_path(module.szExePath,path) && offset<module.modBaseSize)
            loader=reinterpret_cast<LPTHREAD_START_ROUTINE>(module.modBaseAddr+offset);
    }while(Module32NextW(snapshot.value,&module));
    if(loader)return loader;
    SetLastError(ERROR_MOD_NOT_FOUND);return nullptr;
}
}
struct ActiveAttach::Impl {
    struct Candidate {
        DWORD pid=0;std::uint64_t created=0;AttachPolicy policy;
        HANDLE process=nullptr,thread=nullptr;void* remote=nullptr;
        AttachProgress* progress=nullptr;
        ULONGLONG detected=0,issued_at=0,graphics_at=0;
        LONG last_stage=0;
        bool timeout=false,graphics_timeout=false;
        ~Candidate(){
            // Never free a parameter still in use by an outstanding remote call.
            if(remote && process && (!thread || WaitForSingleObject(thread,0)==WAIT_OBJECT_0))
                VirtualFreeEx(process,remote,0,MEM_RELEASE);
            if(thread)CloseHandle(thread);if(process)CloseHandle(process);
        }
    };
    fs::path target,shim;Lease lease;Progress& shared;
    std::map<std::pair<DWORD,std::uint64_t>,std::unique_ptr<Candidate>> candidates;
    std::mutex mutex;std::condition_variable wake;std::vector<Json> events;
    std::atomic<bool> stop{false};std::thread thread;
    Impl(const fs::path& t,const fs::path& s,const Lease& l,Progress& p):target(t),shim(s),lease(l),shared(p) {
        thread=std::thread([this]{run();});
    }
    ~Impl(){stop=true;wake.notify_all();if(thread.joinable())thread.join();}
    void report(const Candidate& c,const char* state,const char* phase,DWORD error=0,const char* detail="") {
        auto value=event("attachment_progress");
        value["state"]=state;value["phase"]=phase;value["pid"]=c.pid;
        value["process_created"]=c.created;value["executable"]=utf8(target.wstring());
        value["attempt"]=c.policy.attempts;value["win32_error"]=error;
        value["elapsed_ms"]=GetTickCount64()-c.detected;value["detail"]=detail;
        std::lock_guard lock(mutex);events.push_back(std::move(value));
    }
    AttachProgress* progress(DWORD pid,std::uint64_t identity) {
        return claim_progress(shared,pid,identity);
    }
    void failed(Candidate& c,const char* phase,DWORD error,const char* detail="") {
        c.policy.finished=true;report(c,"Attach failed",phase,error,detail);
    }
    void scan() {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
        if(snapshot.value==INVALID_HANDLE_VALUE)return;
        PROCESSENTRY32W process{sizeof(process)};
        if(!Process32FirstW(snapshot.value,&process))return;
        do {
            if(stop)return;
            if(_wcsicmp(process.szExeFile,target.filename().c_str()))continue;
            Handle query(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,process.th32ProcessID));
            if(!query.value)continue; // Path cannot yet be established; next scan retries.
            const auto identity=created(query.value);
            if(!identity || candidates.contains({process.th32ProcessID,identity}) || !same_path(image_path(query.value),target.wstring()))continue;
            DWORD session=0;
            if(!ProcessIdToSessionId(process.th32ProcessID,&session) || session!=lease.desktop_session)continue;
            try{if(user_sid(query.value)!=lease.sid)continue;}catch(...){continue;}
            auto c=std::make_unique<Candidate>();c->pid=process.th32ProcessID;c->created=identity;c->detected=GetTickCount64();
            if(identity<lease.started){c->policy.finished=true;report(*c,"Restart required","existing_process");}
            else if(!native_x64(query.value))failed(*c,"architecture",ERROR_BAD_EXE_FORMAT);
            else {
                c->progress=progress(c->pid,identity);
                if(!c->progress)failed(*c,"progress_capacity",ERROR_NOT_ENOUGH_MEMORY);
                else report(*c,"Detected","process_discovered");
            }
            candidates.emplace(std::make_pair(c->pid,identity),std::move(c));
        }while(Process32NextW(snapshot.value,&process));
    }
    void attempt(Candidate& c,ULONGLONG now) {
        c.policy.attempted(now);report(c,"Attaching","open_process");
        auto retry=[&](const char* phase,DWORD error){
            if(c.policy.attempts>=AttachPolicy::max_attempts)failed(c,phase,error);
            else report(c,"Attaching",phase,error);
        };
        c.process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ|SYNCHRONIZE,FALSE,c.pid);
        if(!c.process){retry("open_process",GetLastError());return;}
        auto reset=[&]{if(c.remote){VirtualFreeEx(c.process,c.remote,0,MEM_RELEASE);c.remote=nullptr;}CloseHandle(c.process);c.process=nullptr;};
        if(created(c.process)!=c.created || !same_path(image_path(c.process),target.wstring())){reset();failed(c,"identity_changed",ERROR_INVALID_PARAMETER);return;}
        bool already_attached=false;auto loader=remote_loader(c.pid,already_attached);
        if(already_attached){reset();c.policy.finished=true;report(c,"Already attached","existing_runtime");return;}
        if(!loader){auto error=GetLastError();reset();retry("resolve_loader",error);return;}
        const auto path=shim.wstring();const SIZE_T bytes=(path.size()+1)*sizeof(wchar_t);
        c.remote=VirtualAllocEx(c.process,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
        if(!c.remote){auto error=GetLastError();reset();retry("allocate_path",error);return;}
        SIZE_T written=0;
        if(!WriteProcessMemory(c.process,c.remote,path.c_str(),bytes,&written) || written!=bytes){auto error=GetLastError();reset();retry("write_path",error);return;}
        if(stop){reset();c.policy.finished=true;return;}
        InterlockedCompareExchange(&c.progress->stage,static_cast<LONG>(AttachStage::requested),0);
        c.thread=CreateRemoteThread(c.process,nullptr,0,loader,c.remote,0,nullptr);
        if(!c.thread){auto error=GetLastError();reset();retry("create_loader_thread",error);return;}
        c.policy.issued=true;c.issued_at=now;report(c,"Attaching","remote_load");
    }
    void tick(Candidate& c,ULONGLONG now) {
        if(c.thread && WaitForSingleObject(c.thread,0)==WAIT_OBJECT_0){
            VirtualFreeEx(c.process,c.remote,0,MEM_RELEASE);c.remote=nullptr;
            CloseHandle(c.thread);c.thread=nullptr;
            // A 32-bit thread exit code cannot represent an x64 HMODULE. The
            // target acknowledgement, not this value, determines success.
        }
        if(c.progress){
            const LONG stage=InterlockedCompareExchange(&c.progress->stage,0,0);
            if(stage!=c.last_stage){
                c.last_stage=stage;
                if(stage==static_cast<LONG>(AttachStage::failed))failed(c,"target_initialize",c.progress->error);
                else if(stage==static_cast<LONG>(AttachStage::installing)){
                    c.policy.issued=true;if(!c.issued_at)c.issued_at=now;
                    report(c,"Attaching","target_initialize");
                }
                else if(stage>=static_cast<LONG>(AttachStage::hooks_ready)){
                    c.policy.issued=true;
                    if(!c.graphics_at)c.graphics_at=now;
                    if(stage==static_cast<LONG>(AttachStage::runtime_ready)){
                        c.policy.finished=true;report(c,"Waiting for graphics","runtime_ready");
                    }else report(c,"Waiting for graphics","hooks_ready");
                }
            }
        }
        if(c.policy.finished)return;
        if(c.policy.issued && c.issued_at && !c.graphics_at && now-c.issued_at>=AttachPolicy::remote_ms && !c.timeout){
            c.timeout=true;failed(c,"remote_acknowledgement",WAIT_TIMEOUT,"No target acknowledgement; no duplicate injection will be issued");return;
        }
        if(c.graphics_at && now-c.graphics_at>=AttachPolicy::graphics_ms && !c.graphics_timeout){
            c.graphics_timeout=true;report(c,"Attach failed","graphics_timeout",WAIT_TIMEOUT,"Hooks installed; no graphics session yet");
        }
        if(c.policy.can_attempt(now))attempt(c,now);
    }
    void run() noexcept {
        try {
            while(!stop){
                scan();const auto now=GetTickCount64();
                for(auto it=candidates.begin();it!=candidates.end();){
                    auto& c=*it->second;Handle query(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,c.pid));
                    const bool exited=query.value?(WaitForSingleObject(query.value,0)==WAIT_OBJECT_0 || created(query.value)!=c.created):GetLastError()==ERROR_INVALID_PARAMETER;
                    if(exited){
                        report(c,"Exited","process_exited");
                        if(c.progress){InterlockedExchange(&c.progress->stage,0);InterlockedExchange(&c.progress->pid,0);}
                        it=candidates.erase(it);continue;
                    }
                    if(!stop)tick(c,now);++it;
                }
                std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::milliseconds(50),[this]{return stop.load();});
            }
        }catch(const std::exception& e){
            Candidate c;c.detected=GetTickCount64();report(c,"Attach failed","scanner",ERROR_GEN_FAILURE,e.what());
        }
    }
};
ActiveAttach::ActiveAttach(const fs::path& t,const fs::path& s,const Lease& l,Progress& p):impl_(std::make_unique<Impl>(t,s,l,p)){}
ActiveAttach::~ActiveAttach()=default;
std::vector<Json> ActiveAttach::drain(){std::lock_guard lock(impl_->mutex);std::vector<Json> result;result.swap(impl_->events);return result;}
}
