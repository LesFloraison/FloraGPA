#include "monitor.hpp"
#include "monitor_support.hpp"
#include "hud_model.hpp"
#include <iostream>
#include <map>
#include <set>
#include <atomic>
#include <thread>

namespace flora::capture {
namespace {
using namespace autodetect;
std::atomic<bool> stopping{false};
BOOL WINAPI console_control(DWORD){stopping=true;return TRUE;}
int watchdog(DWORD parent,std::uint64_t identity,const std::wstring& ready_name,const fs::path& root){
    Handle process(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent));
    Handle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,ready_name.c_str()));
    if(!ready.value || !SetEvent(ready.value))win_error("Acknowledge recovery guard");
    if(process.value && created(process.value)==identity)WaitForSingleObject(process.value,INFINITE);
    Handle mutex(CreateMutexW(nullptr,FALSE,mutex_name));if(!mutex.value)win_error("Open recovery lock");
    const auto wait=WaitForSingleObject(mutex.value,10000);if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)return 3;
    try{recover(root/L"recovery.json");}catch(...){ReleaseMutex(mutex.value);throw;}ReleaseMutex(mutex.value);return 0;
}
int worker(const std::wstring& pipe,DWORD client,std::uint64_t client_created){
    Handle owner(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,client));
    if(!owner.value || created(owner.value)!=client_created)throw std::runtime_error("Monitor owner identity changed");
    if(user_sid(owner.value)!=user_sid(GetCurrentProcess()))throw std::runtime_error("Elevation changed the monitor user");
    Handle connection;const auto deadline=GetTickCount64()+15000;
    do{connection.value=CreateFileW(pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);if(connection.value!=INVALID_HANDLE_VALUE)break;Sleep(20);}while(GetTickCount64()<deadline);
    if(connection.value==INVALID_HANDLE_VALUE)win_error("Connect monitor owner");ULONG server=0;
    if(!GetNamedPipeServerProcessId(connection.value,&server) || server!=client)throw std::runtime_error("Monitor pipe identity mismatch");
    Lease* lease=nullptr;Handle mapping;Handle mutex;bool locked=false;fs::path journal;
    try{
        const auto request=receive(connection.value);const auto root=protected_root();journal=root/L"recovery.json";
        Security acl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;"+user_sid(owner.value)+L")");
        mutex.value=CreateMutexW(&acl.attributes,FALSE,mutex_name);if(!mutex.value)win_error("Create registry ownership lock");
        auto wait=WaitForSingleObject(mutex.value,0);if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)throw std::runtime_error("Another Auto-Detect manager is active");locked=true;
        recover(journal);preflight();const auto staged=stage(root);
        const auto shim=staged/L"FloraCaptureShim.dll";wchar_t short_name[32768]{};
        const auto count=GetShortPathNameW(shim.c_str(),short_name,32768);
        if(!count || count>=32768 || wcspbrk(short_name,L" ,\t\r\n"))throw std::runtime_error("AppInit needs a space-free short installation path");
        mapping.value=CreateFileMappingW(INVALID_HANDLE_VALUE,&acl.attributes,PAGE_READWRITE,0,sizeof(Lease),mapping_name);
        if(!mapping.value)win_error("Create Auto-Detect lease");
        lease=static_cast<Lease*>(MapViewOfFile(mapping.value,FILE_MAP_ALL_ACCESS,0,0,sizeof(Lease)));if(!lease)win_error("Map Auto-Detect lease");
        InterlockedExchange(&lease->enabled,0);
        *lease=Lease{};lease->owner_pid=client;lease->owner_created=client_created;lease->broker_pid=GetCurrentProcessId();lease->broker_created=created(GetCurrentProcess());
        ProcessIdToSessionId(client,&lease->desktop_session);FILETIME now{};GetSystemTimeAsFileTime(&now);lease->started=file_time(now);
        const auto output=fs::absolute(wide(request.at("output").get<std::string>()));
        if(fs::exists(output))throw std::runtime_error("Auto-Detect output directory must be new");fs::create_directories(output);
        if(wcscpy_s(lease->runtime,(staged/L"FloraCaptureRuntime.dll").c_str()) || wcscpy_s(lease->output,output.c_str()) || wcscpy_s(lease->sid,user_sid(owner.value).c_str()))throw std::runtime_error("Auto-Detect path is too long");
        const auto mode=request.at("hud").get<std::string>(),key=request.at("hud_key").get<std::string>(),capture=request.at("capture_key").get<std::string>();
        hud::Settings settings;settings.mode=hud::parse_mode(mode);settings.mode_key=hud::parse_hotkey(key);settings.capture_key=hud::parse_hotkey(capture);settings.validate();
        if(strcpy_s(lease->hud,mode.c_str()) || strcpy_s(lease->hud_key,key.c_str()) || strcpy_s(lease->capture_key,capture.c_str()))throw std::runtime_error("HUD setting is too long");
        Registry registry;const auto before=registry.get(L"AppInit_DLLs"),load=registry.get(L"LoadAppInit_DLLs");
        auto text=string_data(before);if(!text.empty())text+=L" ";text+=short_name;const auto installed=string_value(text);
        const auto guard_name=L"Local\\FloraGPA.AutoDetect.Guard."+uuid();
        Handle guard_ready(CreateEventW(&acl.attributes,TRUE,FALSE,guard_name.c_str()));if(!guard_ready.value)win_error("Create recovery guard handshake");
        Handle guard(spawn(staged/L"FloraCapture.exe",L"monitor-guard --parent "+std::to_wstring(GetCurrentProcessId())+L" --created "+std::to_wstring(created(GetCurrentProcess()))+L" --ready "+quote(guard_name),false));
        HANDLE guard_waits[]={guard_ready.value,guard.value};
        if(WaitForMultipleObjects(2,guard_waits,FALSE,10000)!=WAIT_OBJECT_0)throw std::runtime_error("Recovery guard failed to start");
        save(journal,{{"schema",1},{"shim",utf8(short_name)},{"before",before},{"installed",installed},{"load_before",load}});
        registry.put(L"AppInit_DLLs",installed);registry.put(L"LoadAppInit_DLLs",dword_value(1));InterlockedExchange(&lease->enabled,1);
        auto ready=event("ready");ready["state"]="Watching";ready["output_directory"]=utf8(output.wstring());ready["broker_pid"]=GetCurrentProcessId();ready["guard_pid"]=GetProcessId(guard.value);ready["shim"]=utf8(short_name);send(connection.value,ready);
        std::set<std::wstring> seen;auto next_heartbeat=GetTickCount64();
        while(WaitForSingleObject(owner.value,0)==WAIT_TIMEOUT){
            DWORD available=0;if(!PeekNamedPipe(connection.value,nullptr,0,nullptr,&available,nullptr))break;
            if(available){const auto command=receive(connection.value);if(command.value("command",std::string())=="stop")break;}
            for(const auto& directory:fs::directory_iterator(output)){
                if(directory.is_regular_file() && directory.path().extension()==L".failure" && !seen.contains(directory.path().wstring())){
                    Failure failure{};std::ifstream file(directory.path(),std::ios::binary);file.read(reinterpret_cast<char*>(&failure),sizeof(failure));
                    if(file && failure.version==abi_version){
                        failure.executable[1023]=0;auto message=event("attachment_failed");message["pid"]=failure.pid;
                        message["process_created"]=failure.process_created;message["executable"]=utf8(failure.executable);
                        message["error"]="Runtime initialization failed (Win32 "+std::to_string(failure.error)+")";send(connection.value,message);
                        seen.insert(directory.path().wstring());
                    }
                }
                if(!directory.is_directory())continue;const auto manifest=directory.path()/L"session.json";
                if(!fs::is_regular_file(manifest) || seen.contains(manifest.wstring()))continue;
                try{auto session=read(manifest);if(session.at("version")!=protocol_version || session.at("attachment")!="auto_detect")continue;
                    auto message=event("session");message["session"]=session;send(connection.value,message);seen.insert(manifest.wstring());
                }catch(const Json::exception&){/* An incomplete foreign file is not a session. */}
            }
            if(GetTickCount64()>=next_heartbeat){send(connection.value,event("heartbeat"));next_heartbeat=GetTickCount64()+1000;}
            Sleep(100);
        }
        InterlockedExchange(&lease->enabled,0);recover(journal);ReleaseMutex(mutex.value);locked=false;
        auto done=event("stopped");done["state"]="Off";try{send(connection.value,done);}catch(...){}
        UnmapViewOfFile(lease);return 0;
    }catch(const std::exception& e){
        if(lease)InterlockedExchange(&lease->enabled,0);
        std::string error=e.what();if(locked){try{recover(journal);}catch(const std::exception& cleanup){error+="; recovery required: ";error+=cleanup.what();}ReleaseMutex(mutex.value);}
        auto failure=event("error",false);failure["error"]=error;try{send(connection.value,failure);}catch(...){}
        if(lease)UnmapViewOfFile(lease);return 3;
    }
}
}

int monitor_main(int argc,wchar_t** argv){
    using namespace autodetect;
    try{
        std::map<std::wstring,std::wstring> options;
        for(int i=2;i<argc;i+=2){if(i+1>=argc || !options.emplace(argv[i],argv[i+1]).second)throw std::invalid_argument("Missing or duplicate monitor option");}
        const std::wstring verb=argv[1];
        if(verb==L"monitor-recover"){
            wchar_t program_files[MAX_PATH]{};
            if(FAILED(SHGetFolderPathW(nullptr,CSIDL_PROGRAM_FILES,nullptr,SHGFP_TYPE_CURRENT,program_files)))throw std::runtime_error("Find recovery record");
            const auto journal=fs::path(program_files)/L"FloraGPA"/L"AutoDetect"/L"recovery.json";
            if(fs::exists(journal)){
                if(!elevated()){
                    Handle child(spawn(executable(),L"monitor-recover",true));
                    if(WaitForSingleObject(child.value,90000)!=WAIT_OBJECT_0)throw std::runtime_error("Recovery manager timed out");
                    DWORD result=3;if(!GetExitCodeProcess(child.value,&result) || result)throw std::runtime_error("Recovery manager failed");
                }else{
                    Handle mutex(CreateMutexW(nullptr,FALSE,mutex_name));if(!mutex.value)win_error("Open recovery lock");
                    const auto wait=WaitForSingleObject(mutex.value,0);
                    if(wait==WAIT_OBJECT_0 || wait==WAIT_ABANDONED){
                        try{recover(journal);}catch(...){ReleaseMutex(mutex.value);throw;}ReleaseMutex(mutex.value);
                    }else if(wait!=WAIT_TIMEOUT)win_error("Acquire recovery lock");
                    // A live manager owns this journal; another UI must not disarm it.
                }
            }
            std::cout<<event("stopped").dump()<<std::endl;return 0;
        }
        if(verb==L"monitor-guard")return watchdog(std::stoul(options.at(L"--parent")),std::stoull(options.at(L"--created")),options.at(L"--ready"),protected_root());
        if(verb==L"monitor-worker")return worker(options.at(L"--pipe"),std::stoul(options.at(L"--client")),std::stoull(options.at(L"--client-created")));
        for(const auto& [key,value]:options){(void)value;if(key!=L"--out" && key!=L"--hud" && key!=L"--hud-hotkey" && key!=L"--capture-hotkey" && key!=L"--owner-pid")throw std::invalid_argument("Unknown monitor option");}
        const auto output=fs::absolute(options.at(L"--out"));
        hud::Settings settings;
        if(options.contains(L"--hud"))settings.mode=hud::parse_mode(utf8(options.at(L"--hud")));
        if(options.contains(L"--hud-hotkey"))settings.mode_key=hud::parse_hotkey(utf8(options.at(L"--hud-hotkey")));
        if(options.contains(L"--capture-hotkey"))settings.capture_key=hud::parse_hotkey(utf8(options.at(L"--capture-hotkey")));
        settings.validate();
        Handle ui;if(options.contains(L"--owner-pid")){ui.value=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,std::stoul(options.at(L"--owner-pid")));if(!ui.value)win_error("Identify Auto-Detect UI");if(user_sid(ui.value)!=user_sid(GetCurrentProcess()))throw std::runtime_error("Auto-Detect UI owner mismatch");}
        const auto name=L"\\\\.\\pipe\\FloraGPA.AutoDetect."+uuid();Security acl(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+user_sid(GetCurrentProcess())+L")");
        Handle pipe(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,max_message,max_message,0,&acl.attributes));
        if(pipe.value==INVALID_HANDLE_VALUE)win_error("Create Auto-Detect control pipe");
        Handle connected(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED ov{};ov.hEvent=connected.value;
        if(!ConnectNamedPipe(pipe.value,&ov) && GetLastError()!=ERROR_IO_PENDING)win_error("Listen for Auto-Detect manager");
        Handle child(spawn(executable(),L"monitor-worker --pipe "+quote(name)+L" --client "+std::to_wstring(GetCurrentProcessId())+L" --client-created "+std::to_wstring(created(GetCurrentProcess())),!elevated()));
        HANDLE waits[]={connected.value,child.value};const auto wait=WaitForMultipleObjects(2,waits,FALSE,90000);
        if(wait!=WAIT_OBJECT_0){CancelIoEx(pipe.value,&ov);DWORD ignored=0;GetOverlappedResult(pipe.value,&ov,&ignored,TRUE);throw std::runtime_error("Auto-Detect manager did not connect");}
        DWORD ignored=0;if(!GetOverlappedResult(pipe.value,&ov,&ignored,TRUE))win_error("Connect Auto-Detect manager");
        ULONG client=0;if(!GetNamedPipeClientProcessId(pipe.value,&client) || client!=GetProcessId(child.value))throw std::runtime_error("Auto-Detect manager identity mismatch");
        send(pipe.value,{{"output",utf8(output.wstring())},{"hud",hud::mode_name(settings.mode)},{"hud_key",hud::hotkey_name(settings.mode_key)},{"capture_key",hud::hotkey_name(settings.capture_key)}});
        SetConsoleCtrlHandler(console_control,TRUE);
        std::thread([]{std::string line;while(std::getline(std::cin,line)){if(line=="stop")break;}stopping=true;}).detach();
        bool stop_sent=false,failed=false,first_message=true;
        for(;;){
            if((stopping || (ui.value && ui.value!=INVALID_HANDLE_VALUE && WaitForSingleObject(ui.value,0)==WAIT_OBJECT_0)) && !stop_sent){send(pipe.value,{{"command","stop"}});stop_sent=true;}
            auto message=receive(pipe.value,first_message?45000:io_timeout);first_message=false;std::cout<<message.dump()<<std::endl;
            const auto type=message.value("event",std::string());if(type=="error"){failed=true;break;}if(type=="stopped")break;
        }
        WaitForSingleObject(child.value,10000);return failed?3:0;
    }catch(const std::exception& e){auto failure=event("error",false);failure["error"]=e.what();std::cout<<failure.dump()<<std::endl;return 3;}
}
}
