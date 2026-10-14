#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <string>
namespace flora::diagnostic {
inline std::vector<std::string> processModules(){
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(snapshot==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot snapshot process modules");
    std::vector<std::string> result;
    try{
        MODULEENTRY32W entry{};entry.dwSize=sizeof entry;
        if(!Module32FirstW(snapshot,&entry))throw std::runtime_error("Cannot enumerate first process module");
        do{const auto path=std::filesystem::path(entry.szExePath).generic_u8string();result.emplace_back(path.begin(),path.end());}while(Module32NextW(snapshot,&entry));
        if(GetLastError()!=ERROR_NO_MORE_FILES)throw std::runtime_error("Incomplete module enumeration");
    }catch(...){CloseHandle(snapshot);throw;}
    CloseHandle(snapshot);return result;
}
}
