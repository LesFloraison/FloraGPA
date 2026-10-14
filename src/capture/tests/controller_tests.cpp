#include "protocol.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flora::capture;
namespace fs=std::filesystem;
namespace {
// Windows can report ACCESS_DENIED while the peer's signal deletion is pending.
// Retry that transient observation; do not restart the target or alter outcomes.
bool signal_exists(const fs::path& path){
    for(unsigned attempt=0;attempt<100;++attempt){std::error_code error;const bool found=fs::exists(path,error);
        if(!error)return found;
        if(error.value()!=ERROR_ACCESS_DENIED && error.value()!=ERROR_SHARING_VIOLATION)throw fs::filesystem_error("Observe target signal",path,error);
        Sleep(5);
    }throw std::runtime_error("Target signal observation remained inaccessible");
}

void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
std::wstring quote(const std::wstring& value){std::wstring r=L"\"";unsigned n=0;for(auto c:value){if(c==L'\\'){++n;continue;}r.append(c==L'"'?2*n+1:n,L'\\');n=0;r+=c;}r.append(2*n,L'\\');return r+L'"';}
void signal(const fs::path& p){std::ofstream f(p);require(bool(f),"Cannot signal target");}
struct Harness {
    fs::path controller,target,out,session,oracle;unsigned step=0;Handle process;
    Harness(fs::path c,fs::path t,fs::path o):controller(std::move(c)),target(std::move(t)),out(std::move(o)),session(out/L"session"),oracle(out/L"target") {fs::create_directories(out);}
    ~Harness(){if(process.value!=INVALID_HANDLE_VALUE){try{signal(oracle/L"finish.signal");WaitForSingleObject(process.value,10000);}catch(...){}}}
    Json run(const std::vector<std::wstring>& args,int expected=0){
        const auto path=out/(L"step-"+std::to_wstring(++step)+L".json");SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle log(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));require(log.value!=INVALID_HANDLE_VALUE,"Cannot create CLI log");
        Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
        std::wstring command=quote(controller.wstring());for(const auto& arg:args)command+=L" "+quote(arg);
        STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=input.value;si.hStdOutput=si.hStdError=log.value;PROCESS_INFORMATION pi{};
        require(CreateProcessW(controller.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi),"Cannot start capture CLI");Handle child(pi.hProcess),thread(pi.hThread);
        require(WaitForSingleObject(child.value,30000)==WAIT_OBJECT_0,"CLI timeout");DWORD code=0;GetExitCodeProcess(child.value,&code);
        std::ifstream file(path);Json result;file>>result;require(expected<0?(code==0 || code==3):int(code)==expected,("Unexpected CLI result: "+result.dump()).c_str());return result;
    }
    Json call(const wchar_t* verb,int expected=0,const std::wstring& id=L""){std::vector<std::wstring> args{verb,L"--session",(session/L"session.json").wstring()};if(!id.empty()){args.push_back(L"--swap-chain");args.push_back(id);}return run(args,expected);}
    void wait_file(const wchar_t* name){const auto end=GetTickCount64()+20000;while(!signal_exists(oracle/name)){require(!signal_exists(oracle/L"error.json"),"Live target error");require(GetTickCount64()<end,"Target signal timeout");Sleep(10);}}
    void launch(unsigned mode){const auto r=run({L"launch",L"--exe",target.wstring(),L"--out",session.wstring(),L"--",oracle.wstring(),std::to_wstring(mode)});process.value=OpenProcess(SYNCHRONIZE,FALSE,r.at("pid").get<DWORD>());require(process.value!=nullptr,"Cannot observe target lifetime");wait_file(L"ready.json");}
    void pause(){signal(oracle/L"pause.signal");wait_file(L"paused.signal");}
    void resume(){fs::remove(oracle/L"pause.signal");const auto end=GetTickCount64()+5000;while(signal_exists(oracle/L"paused.signal")){require(GetTickCount64()<end,"Resume timeout");Sleep(5);}}
    void reject_bad_identity(){
        std::ifstream file(session/L"session.json");Json manifest;file>>manifest;
        for(unsigned mode=0;mode<15;++mode){
            const auto name=pipe_name(manifest["session_id"].get<std::string>());Handle pipe;
            const auto deadline=GetTickCount64()+5000;
            do{pipe.value=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);if(pipe.value!=INVALID_HANDLE_VALUE)break;Sleep(5);}while(GetTickCount64()<deadline);
            require(pipe.value!=INVALID_HANDLE_VALUE,"Cannot open identity test pipe");
            Json bad={{"version",protocol_version},{"session_id",manifest["session_id"]},{"target_pid",manifest["pid"]},{"client_pid",GetCurrentProcessId()},{"request_id","00000000-0000-4000-8000-000000000001"},{"command","capture"}};
            if(mode==0)bad["version"]=1;else if(mode==1)bad["client_pid"]=0;else if(mode==2)bad["target_pid"]=0;else if(mode==3)bad["session_id"]="00000000-0000-4000-8000-000000000002";else if(mode==4)bad["version"]=uint64_t(0x100000002);else if(mode==5)bad["version"]=2.0;else if(mode==6)bad["swap_chain"]=0;else if(mode==7)bad["swap_chain"]=-1;
            else if(mode==8)bad["resource_limit_mib"]=0;else if(mode==9)bad["resource_limit_mib"]=4097;
            else if(mode==10)bad["resource_limit_mib"]=1.0;else if(mode==11)bad["resource_limit_mib"]="1024";
            else if(mode==12)bad["resource_limit_mib"]=-1;else if(mode==13)bad["resource_limit_mib"]=UINT64_MAX;
            else{bad["command"]="status";bad["resource_limit_mib"]=1024;}
            send(pipe.value,bad);const auto answer=receive(pipe.value);send(pipe.value,Json{{"request_id",bad["request_id"]}});
            require(!answer["ok"].get<bool>() && answer["error"]==(mode==0 || mode==4 || mode==5?"protocol_version_mismatch":mode>=8?"invalid_resource_limit":mode>=6?"invalid_swap_chain":"session_process_mismatch"),"Invalid IPC identity accepted");
            std::ofstream log(out/(L"identity-"+std::to_wstring(mode)+L".json"));log<<answer.dump(2);
        }
    }
    Json settle(){const auto end=GetTickCount64()+15000;for(;;){auto r=call(L"status");if(!r.at("capture_progress").value("busy",false))return r;require(GetTickCount64()<end,"Capture settlement timeout");Sleep(20);}}
};
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==4,"Expected controller, target and new output directory");const auto out=fs::absolute(argv[3]);require(!signal_exists(out),"Output exists");Json results=Json::array();
#if defined(FLORA_DEFERRED_TEXTURE_CONTROLLER)
    constexpr unsigned first_mode=28,last_mode=29;
#elif defined(FLORA_DEFERRED_NOOVERWRITE_CONTROLLER)
    constexpr unsigned first_mode=27,last_mode=28;
#elif defined(FLORA_DEFERRED_DISCARD_CONTROLLER)
    constexpr unsigned first_mode=26,last_mode=27;
#elif defined(FLORA_MSAA_CONTROLLER)
    constexpr unsigned first_mode=24,last_mode=26;
#else
    constexpr unsigned first_mode=0,last_mode=24;
#endif
    for(unsigned mode=first_mode;mode<last_mode;++mode){
        Harness h(fs::absolute(argv[1]),fs::absolute(argv[2]),out/std::to_wstring(mode));h.launch(mode);h.reject_bad_identity();
        if(mode==first_mode){
            for(const auto* invalid:{L"0",L"4097",L"-1",L"1.0",L"18446744073709551616"})
                require(!h.run({L"capture",L"--session",(h.session/L"session.json").wstring(),L"--resource-limit-mib",invalid},2)["ok"].get<bool>(),"Invalid CLI limit accepted");
            require(!h.run({L"status",L"--session",(h.session/L"session.json").wstring(),L"--resource-limit-mib",L"4"},2)["ok"].get<bool>(),"Status accepted a capture-only limit");
        }
        auto status=h.call(L"status");require(status["capture_limits"]["default_resource_limit_mib"]==1024 && status["capture_limits"]["max_resource_limit_mib"]==4096,"Capture budget capabilities missing");require(status["capture_available"],"Live target profile was not available");const auto id=std::to_wstring(status["swap_chains"]["active"][0]["id"].get<uint64_t>());
        h.pause();h.call(L"capture",0,id);require(h.call(L"capture",3,id)["error"]=="busy","Duplicate request was not busy");require(h.call(L"cancel")["cancelled"],"Armed cancel failed");h.resume();
        Json frames=Json::array();
        for(unsigned frame=0;frame<3;++frame){
            const uint32_t limit=frame==0?4:frame==1?4096:1024;
            if(frame<2)h.run({L"capture",L"--session",(h.session/L"session.json").wstring(),L"--swap-chain",id,L"--resource-limit-mib",std::to_wstring(limit)});
            else h.call(L"capture",0,id);
            const auto done=h.settle();require(done["last_capture"]["resource_limit_mib"]==limit,"Request resource budget not preserved/reset");require(done["last_capture"]["state"]=="Completed",done.dump().c_str());const auto path=fs::u8path(done["last_capture"]["path"].get<std::string>());require(fs::is_regular_file(path),"Completed frame missing");frames.push_back(done["last_capture"]);}
        // Default selection is environment-sensitive; verify either the unique
        // foreground result or a specific request to select explicitly.
        const auto automatic=h.call(L"capture",-1);bool foreground=automatic["ok"].get<bool>();
        if(foreground){require(automatic["swap_chains"]["active"][0]["foreground"],"Default selected a non-foreground swap");const auto done=h.settle();require(done["last_capture"]["state"]=="Completed","Automatic foreground capture failed");frames.push_back(done["last_capture"]);}
        else require(automatic["error"]=="swap_chain_selection_required","Ambiguous default did not request selection");
        h.pause();signal(h.oracle/L"update.signal");h.call(L"capture",0,id);h.resume();const auto failed=h.settle();require(failed["last_capture"]["state"]=="Failed" && failed["last_capture"].value("error",std::string()).find("UpdateSubresource")!=std::string::npos,"Unsupported empty-box update was not identified");fs::remove(h.oracle/L"update.signal");
        h.pause();h.call(L"capture",0,id);h.resume();const auto recovered=h.settle();require(recovered["last_capture"]["state"]=="Completed","Capture did not recover after rejection");frames.push_back(recovered["last_capture"]);
        h.pause();h.call(L"capture",0,id);require(h.call(L"disconnect")["state"]=="Disconnected","Disconnect not reported");h.resume();Sleep(100);status=h.call(L"status");require(status["state"]=="Disconnected" && !status["capture_available"].get<bool>(),"Disconnected status is incorrect");require(h.call(L"capture",3,id)["error"]=="disconnected","Disconnected capture accepted");require(WaitForSingleObject(h.process.value,0)==WAIT_TIMEOUT,"Disconnect killed target");
        signal(h.oracle/L"finish.signal");require(WaitForSingleObject(h.process.value,10000)==WAIT_OBJECT_0,"Target did not exit");require(h.call(L"status",3)["state"]=="Exited","Target exit not reported");
        for(const auto& entry:fs::directory_iterator(h.session))require(entry.path().extension()!=L".partial","Transaction leaked partial output");
        std::ifstream exit_file(h.oracle/L"exit.json");Json exit;exit_file>>exit;
        require(exit["identity_checks"]==exit["frames"],"Backbuffer identity observations missing");if(mode>=2 && mode!=8 && mode!=24)require(exit["unbind_checks"]==exit["frames"],"Flip unbind observations missing");
        if(mode==24 || mode==25)require(exit["msaa_native_cpu_exact"],"Native MSAA CPU oracle failed after disconnect");
        results.push_back({{"mode",mode},{"frames",frames},{"target_exit",exit},{"automatic_foreground",foreground},{"failure",failed["last_capture"]},{"module_status",status}});
    }
    std::ofstream report(out/L"results.json");report<<results.dump(2);std::cout<<Json{{"passed",true},{"launches",last_mode-first_mode},{"scope","Startup injection and native private IPC"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
