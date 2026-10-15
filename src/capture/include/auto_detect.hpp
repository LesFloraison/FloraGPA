#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <d3d11.h>
#include <dxgi1_3.h>

namespace flora::capture::autodetect {
inline constexpr wchar_t mapping_name[]=L"Local\\FloraGPA.AutoDetect.v1";
inline constexpr wchar_t mutex_name[]=L"Global\\FloraGPA.AutoDetect.Registry.v1";
inline constexpr wchar_t registry_key[]=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows";
inline constexpr DWORD abi_version=1;
struct Lease {
    DWORD version=abi_version;
    volatile LONG enabled=0;
    DWORD desktop_session=0, broker_pid=0, owner_pid=0;
    std::uint64_t started=0, broker_created=0, owner_created=0;
    wchar_t sid[184]{};
    wchar_t runtime[1024]{};
    wchar_t output[4096]{};
    char hud[32]{}, hud_key[64]{}, capture_key[64]{};
};
struct Failure {
    DWORD version=abi_version,pid=0,error=0;
    std::uint64_t process_created=0;
    wchar_t executable[1024]{};
};
// The shim owns the five entry detours. Runtime uses their trampolines and
// returns wrappers; neither side patches an entry owned by the other.
struct Entries {
    decltype(&D3D11CreateDevice) device=nullptr;
    decltype(&D3D11CreateDeviceAndSwapChain) swap=nullptr;
    decltype(&CreateDXGIFactory) factory=nullptr;
    decltype(&CreateDXGIFactory1) factory1=nullptr;
    decltype(&CreateDXGIFactory2) factory2=nullptr;
};
using Initialize=DWORD (WINAPI*)(DWORD,const Lease*,const Entries*,Entries*);
inline std::uint64_t file_time(FILETIME value) noexcept {
    return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
inline std::uint64_t created(HANDLE process) noexcept {
    FILETIME c{},e{},k{},u{};
    return GetProcessTimes(process,&c,&e,&k,&u)?file_time(c):0;
}
inline bool excluded(const wchar_t* path) noexcept {
    const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
    constexpr const wchar_t* prefixes[]={L"FloraGPA",L"FloraCapture",L"FloraAutoDetect",L"GpaMonitor",L"GpaServer",
        L"frameanalyzer",L"systemanalyzer",L"renderdoc",L"UnityCrashHandler",L"crashpad",L"bugrpt",
        L"NEPDaemon",L"KernelDumpAnalyzer"};
    for(auto prefix:prefixes)if(_wcsnicmp(name,prefix,wcslen(prefix))==0)return true;
    constexpr const wchar_t* executables[]={L"steam.exe",L"steamservice.exe",L"steamwebhelper.exe",L"gameoverlayui.exe",
        L"steamerrorreporter64.exe",L"PCLauncher.exe",L"PCUpdater.exe",L"chrome.exe",L"msedge.exe",L"firefox.exe",
        L"QtWebEngineProcess.exe",L"Code.exe",L"Codex.exe",L"codex-computer-use.exe",L"devenv.exe",L"MSBuild.exe",L"dllhost.exe",
        L"rundll32.exe",L"explorer.exe",L"conhost.exe"};
    for(auto executable:executables)if(_wcsicmp(name,executable)==0)return true;
    wchar_t windows[MAX_PATH]{};const auto n=GetWindowsDirectoryW(windows,MAX_PATH);
    return n && n<MAX_PATH && _wcsnicmp(path,windows,n)==0 && path[n]==L'\\';
}
}
