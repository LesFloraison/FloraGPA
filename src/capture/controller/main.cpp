#include "protocol.hpp"
#include <detours.h>
#include <objbase.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

using namespace flora::capture;
namespace fs=std::filesystem;
namespace {
std::string utf8(const std::wstring& value){
    if(value.empty())return {};
    int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!length)win_error("Encode UTF-8");
    std::string result(length,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),length,nullptr,nullptr);return result;
}
std::string uuid(){
    GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Cannot create session ID");
    wchar_t raw[40]{};StringFromGUID2(id,raw,40);std::wstring result(raw+1,36);
    std::transform(result.begin(),result.end(),result.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});return utf8(result);
}
std::wstring quote(const std::wstring& value){
    std::wstring result=L"\"";unsigned slashes=0;
    for(wchar_t c:value){
        if(c==L'\\'){++slashes;continue;}
        result.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;result+=c;
    }
    result.append(slashes*2,L'\\');return result+L'"';
}
void ensure_x64(const fs::path& path){
    std::ifstream f(path,std::ios::binary);IMAGE_DOS_HEADER dos{};
    if(!f.read(reinterpret_cast<char*>(&dos),sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0)
        throw std::invalid_argument("Target is not a PE executable");
    f.seekg(dos.e_lfanew);DWORD signature=0;IMAGE_FILE_HEADER header{};
    if(!f.read(reinterpret_cast<char*>(&signature),sizeof(signature)) || signature!=IMAGE_NT_SIGNATURE ||
       !f.read(reinterpret_cast<char*>(&header),sizeof(header)) || header.Machine!=IMAGE_FILE_MACHINE_AMD64)
        throw std::invalid_argument("Only Windows x64 targets are supported");
}
std::vector<wchar_t> environment(const std::string& session){
    LPWCH raw=GetEnvironmentStringsW();if(!raw)win_error("Read environment");
    std::vector<std::wstring> entries;
    try{
        for(const wchar_t* p=raw;*p;p+=wcslen(p)+1)
            if(_wcsnicmp(p,L"FLORAGPA_CAPTURE_SESSION=",24)!=0)entries.emplace_back(p);
    }catch(...){FreeEnvironmentStringsW(raw);throw;}
    FreeEnvironmentStringsW(raw);
    entries.push_back(L"FLORAGPA_CAPTURE_SESSION="+std::wstring(session.begin(),session.end()));
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    std::vector<wchar_t> result;
    for(const auto& entry:entries){result.insert(result.end(),entry.begin(),entry.end());result.push_back(0);}
    result.push_back(0);return result;
}
Json command(const std::string& session,const std::string& verb,DWORD pid, const Json& options=Json::object()){
    Json message={{"version",protocol_version},{"request_id",uuid()},{"command",verb}};
    message.update(options);return request(session,message,pid);
}
uint64_t creation_time(HANDLE process){
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(process,&created,&exited,&kernel,&user))win_error("Identify target process creation");
    return (uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
}
void save(const fs::path& path,const Json& value){
    std::ofstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot write session manifest");
    file<<value.dump(2)<<'\n';file.close();if(!file)throw std::runtime_error("Cannot finish session manifest");
}
struct InheritedHandles {
    std::vector<std::uint8_t> storage;
    HANDLE handles[2]{};
    LPPROC_THREAD_ATTRIBUTE_LIST list=nullptr;
    InheritedHandles(HANDLE input,HANDLE output){
        SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
        storage.resize(size);list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if(!InitializeProcThreadAttributeList(list,1,0,&size)){list=nullptr;win_error("Initialize target handle list");}
        handles[0]=input;handles[1]=output;
        if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)){
            const auto error=GetLastError();DeleteProcThreadAttributeList(list);list=nullptr;SetLastError(error);win_error("Limit inherited target handles");
        }
    }
    ~InheritedHandles(){if(list)DeleteProcThreadAttributeList(list);}
};
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc<2 || std::wstring(argv[1])==L"--help"){
            std::cout<<"FloraCapture native bootstrap (frame capture is not yet available)\n"
                     <<"launch --exe FILE --out NEW_DIRECTORY [--cwd DIRECTORY] [-- target arguments]\n"
                     <<"status|capture|cancel|disconnect --session SESSION_JSON [capture: --swap-chain ID]\n";
            return argc<2?2:0;
        }
        const std::wstring verb=argv[1];std::map<std::wstring,std::wstring> options;std::vector<std::wstring> arguments;
        for(int i=2;i<argc;++i){
            std::wstring key=argv[i];
            if(key==L"--"){while(++i<argc)arguments.emplace_back(argv[i]);break;}
            if(key!=L"--exe" && key!=L"--out" && key!=L"--cwd" && key!=L"--session" && key!=L"--swap-chain")throw std::invalid_argument("Unknown option");
            if(++i>=argc || !options.emplace(key,argv[i]).second)throw std::invalid_argument("Missing or duplicate option value");
        }
        if(verb!=L"launch"){
            if(verb!=L"status" && verb!=L"capture" && verb!=L"cancel" && verb!=L"disconnect")throw std::invalid_argument("Unknown command");
            const bool selected=options.count(L"--swap-chain")!=0;
            if(options.size()!=(selected?2u:1u) || !options.count(L"--session") || !arguments.empty() || (selected && verb!=L"capture"))
                throw std::invalid_argument("Expected --session and optional capture --swap-chain");
            Json extra=Json::object();
            if(selected){
                const auto& text=options.at(L"--swap-chain");
                if(text.empty() || text.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::invalid_argument("Invalid swap-chain ID");
                const auto id=std::stoull(text);if(!id)throw std::invalid_argument("Invalid swap-chain ID");extra["swap_chain"]=id;
            }
            std::ifstream file(fs::path(options.at(L"--session")));Json session;file>>session;
            if(session.value("version",0u)!=protocol_version)throw std::invalid_argument("Session protocol mismatch");
            Handle process(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,session.at("pid").get<DWORD>()));
            if(!process.value && GetLastError()!=ERROR_INVALID_PARAMETER)win_error("Open capture target");
            if(!process.value || WaitForSingleObject(process.value,0)==WAIT_OBJECT_0){
                std::cout<<Json{{"ok",false},{"state","Exited"},{"error","target_exited"}}.dump()<<'\n';return 3;
            }
            if(!session.contains("process_created") || creation_time(process.value)!=session.at("process_created").get<uint64_t>()){
                std::cout<<Json{{"ok",false},{"state","Exited"},{"error","session_process_mismatch"}}.dump()<<'\n';return 3;
            }
            const auto result=command(session.at("session_id"),utf8(verb),session.at("pid").get<DWORD>(),extra);std::cout<<result.dump()<<'\n';return result.value("ok",false)?0:3;
        }
        if(!options.count(L"--exe") || !options.count(L"--out") || options.count(L"--session") || options.count(L"--swap-chain"))throw std::invalid_argument("launch requires --exe and --out");
        const auto exe=fs::absolute(options.at(L"--exe"));ensure_x64(exe);
        const auto cwd=options.count(L"--cwd")?fs::absolute(options.at(L"--cwd")):exe.parent_path();
        if(!fs::is_directory(cwd))throw std::invalid_argument("Target working directory does not exist");
        wchar_t module[32768]{};if(!GetModuleFileNameW(nullptr,module,32768))win_error("Find capture runtime");
        const auto runtime=fs::path(module).parent_path()/L"FloraCaptureRuntime.dll";ensure_x64(runtime);
        const auto dll=utf8(runtime.wstring());
        if(std::any_of(dll.begin(),dll.end(),[](unsigned char c){return c>127;}))throw std::invalid_argument("Detours bootstrap currently requires an ASCII runtime installation path");
        const auto out=fs::absolute(options.at(L"--out"));
        if(fs::exists(out))throw std::invalid_argument("Session directory must be new");
        fs::create_directories(out);
        const auto session=uuid();auto env=environment(session);
        std::wstring line=quote(exe.wstring());for(const auto& arg:arguments)line+=L" "+quote(arg);
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);PROCESS_INFORMATION child{};
        // The target must not keep the controller's redirected stdout/stderr
        // alive: subprocess clients otherwise wait until the game exits.
        startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherited,
                                 OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(input.value==INVALID_HANDLE_VALUE)win_error("Create target input");
        startup.StartupInfo.hStdInput=input.value;
        Handle target_log(CreateFileW((out/L"target.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                                     &inherited,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(target_log.value==INVALID_HANDLE_VALUE)win_error("Create target log");
        startup.StartupInfo.hStdOutput=target_log.value;startup.StartupInfo.hStdError=target_log.value;
        InheritedHandles handles(input.value,target_log.value);startup.lpAttributeList=handles.list;
        if(!DetourCreateProcessWithDllExW(exe.c_str(),line.data(),nullptr,nullptr,TRUE,
            CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,env.data(),cwd.c_str(),
            &startup.StartupInfo,&child,dll.c_str(),nullptr))win_error("Start injected target");
        Handle process(child.hProcess),thread(child.hThread);
        Json manifest={{"version",protocol_version},{"session_id",session},{"pid",child.dwProcessId},
                       {"process_created",creation_time(process.value)},
                       {"executable",utf8(exe.wstring())},{"runtime",utf8(runtime.wstring())},
                       {"capture_available",false},{"milestone","native_bootstrap_only"}};
        try{save(out/L"session.json",manifest);}catch(...){TerminateProcess(process.value,ERROR_WRITE_FAULT);throw;}
        if(ResumeThread(thread.value)==DWORD(-1)){TerminateProcess(process.value,ERROR_INVALID_FUNCTION);win_error("Resume injected target");}
        Json status;std::string error;
        const auto deadline=GetTickCount64()+10000;
        do {
            if(WaitForSingleObject(process.value,0)==WAIT_OBJECT_0){error="Target exited before handshake";break;}
            try{status=command(session,"status",child.dwProcessId);break;}catch(const std::exception& e){error=e.what();Sleep(50);}
        }while(GetTickCount64()<deadline);
        manifest["session_file"]=utf8((out/L"session.json").wstring());manifest["status"]=status;
        manifest["ok"]=status.is_object() && status.value("ok",false);
        if(!manifest["ok"].get<bool>())manifest["error"]=status.is_object()?status.value("error",std::string("Session handshake rejected")):error;
        save(out/L"launch.json",manifest);std::cout<<manifest.dump()<<'\n';return manifest["ok"].get<bool>()?0:3;
    }catch(const std::exception& e){std::cout<<Json{{"ok",false},{"error",e.what()}}.dump()<<'\n';return 2;}
}
