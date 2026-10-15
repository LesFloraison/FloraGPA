#pragma once
#include "capture_limits.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace flora::capture {
using Json=nlohmann::json;
inline constexpr std::uint32_t protocol_version=4;
inline constexpr std::uint32_t max_message=65536;
inline constexpr DWORD io_timeout=5000;
class Handle {
public:
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h) {}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE && value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
[[noreturn]] inline void win_error(const char* operation){
    throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation);
}
inline std::wstring pipe_name(const std::string& session){
    if(session.size()!=36 || session.find_first_not_of("0123456789abcdef-")!=std::string::npos)
        throw std::invalid_argument("Invalid session identifier");
    for(std::size_t i=0;i<session.size();++i)
        if((session[i]=='-')!=(i==8 || i==13 || i==18 || i==23))throw std::invalid_argument("Invalid session UUID");
    return L"\\\\.\\pipe\\FloraGPA.Capture."+std::wstring(session.begin(),session.end());
}
inline DWORD complete(HANDLE handle,OVERLAPPED& ov,DWORD timeout=io_timeout){
    if(WaitForSingleObject(ov.hEvent,timeout)!=WAIT_OBJECT_0){
        CancelIoEx(handle,&ov);
        DWORD ignored=0;GetOverlappedResult(handle,&ov,&ignored,TRUE);
        throw std::runtime_error("Capture IPC timed out");
    }
    DWORD transferred=0;
    if(!GetOverlappedResult(handle,&ov,&transferred,FALSE))win_error("Capture IPC completion");
    return transferred;
}
inline void transfer(HANDLE handle,void* buffer,std::uint32_t size,bool write,DWORD timeout=io_timeout){
    auto bytes=static_cast<std::uint8_t*>(buffer);
    while(size){
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.value)win_error("Create IPC event");
        OVERLAPPED ov{};ov.hEvent=event.value;DWORD n=0;
        BOOL ok=write?WriteFile(handle,bytes,size,&n,&ov):ReadFile(handle,bytes,size,&n,&ov);
        if(!ok){if(GetLastError()!=ERROR_IO_PENDING)win_error("Capture IPC transfer");n=complete(handle,ov,timeout);}
        if(!n || n>size)throw std::runtime_error("Capture IPC closed or invalid transfer");
        bytes+=n;size-=n;
    }
}
inline void send(HANDLE handle,const Json& message){
    std::string bytes=message.dump();
    if(bytes.empty() || bytes.size()>max_message)throw std::invalid_argument("Capture message too large");
    auto length=static_cast<std::uint32_t>(bytes.size());transfer(handle,&length,4,true);
    transfer(handle,bytes.data(),length,true);
}
inline Json receive(HANDLE handle,DWORD timeout=io_timeout){
    std::uint32_t length=0;transfer(handle,&length,4,false,timeout);
    if(!length || length>max_message)throw std::invalid_argument("Invalid capture message length");
    std::string bytes(length,'\0');transfer(handle,bytes.data(),length,false,timeout);
    return Json::parse(bytes);
}
inline Json request(const std::string& session,const Json& message,DWORD expected_pid){
    const auto name=pipe_name(session);
    Handle pipe;const auto deadline=GetTickCount64()+io_timeout;
    do {
        pipe.value=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(pipe.value!=INVALID_HANDLE_VALUE)break;
        const auto error=GetLastError();
        if(error!=ERROR_PIPE_BUSY && error!=ERROR_FILE_NOT_FOUND)win_error("Connect capture session");
        Sleep(10);
    }while(GetTickCount64()<deadline);
    if(pipe.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Capture session unavailable");
    ULONG server_pid=0;
    if(!GetNamedPipeServerProcessId(pipe.value,&server_pid))win_error("Identify capture session");
    if(server_pid!=expected_pid)throw std::runtime_error("Capture session process identity mismatch");
    auto authenticated=message;authenticated["session_id"]=session;authenticated["client_pid"]=GetCurrentProcessId();authenticated["target_pid"]=expected_pid;
    send(pipe.value,authenticated);auto answer=receive(pipe.value);
    if(answer.value("session_id",std::string())!=session || answer.value("pid",0u)!=expected_pid || answer.value("version",0u)!=protocol_version || answer.value("request_id",std::string())!=message.value("request_id",std::string()))
        throw std::runtime_error("Capture response identity mismatch");
    send(pipe.value,Json{{"request_id",message.value("request_id",std::string())}});
    return answer;
}
} // namespace flora::capture
