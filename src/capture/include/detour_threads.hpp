#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <detours.h>

namespace flora::capture {
// Collect handles before starting a transaction. Enlist only after preparing
// patches, minimizing the interval during which other application threads stop.
class DetourThreads {
    HANDLE handles_[2048]{};
    unsigned count_=0;
    LONG error_=NO_ERROR;
public:
    explicit DetourThreads(bool concurrent=true) {
        if(!concurrent)return;
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snapshot==INVALID_HANDLE_VALUE){error_=GetLastError();return;}
        THREADENTRY32 entry{sizeof(entry)};
        if(!Thread32First(snapshot,&entry)){error_=GetLastError();CloseHandle(snapshot);return;}
        do {
            if(entry.th32OwnerProcessID!=GetCurrentProcessId() || entry.th32ThreadID==GetCurrentThreadId())continue;
            if(count_==2048){error_=ERROR_NOT_ENOUGH_MEMORY;break;}
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);
            if(thread){
                if(GetProcessIdOfThread(thread)==GetCurrentProcessId())handles_[count_++]=thread;
                else CloseHandle(thread); // A thread ID can be reused after the snapshot.
            }
            else if(GetLastError()!=ERROR_INVALID_PARAMETER){error_=GetLastError();break;}
        }while(Thread32Next(snapshot,&entry));
        CloseHandle(snapshot);
    }
    ~DetourThreads(){for(unsigned i=0;i<count_;++i)CloseHandle(handles_[i]);}
    LONG enlist(){
        if(error_!=NO_ERROR)return error_;
        for(unsigned i=0;i<count_;++i){
            DWORD code=0;if(GetExitCodeThread(handles_[i],&code) && code!=STILL_ACTIVE)continue;
            const auto result=DetourUpdateThread(handles_[i]);if(result!=NO_ERROR)return result;
        }
        return NO_ERROR;
    }
};
}
