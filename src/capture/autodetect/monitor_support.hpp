#pragma once
#include "auto_detect.hpp"
#include "registry_model.hpp"
#include "protocol.hpp"
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace flora::capture::autodetect {
namespace fs=std::filesystem;
inline std::string utf8(const std::wstring& text){
    if(text.empty())return {};const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(!n)win_error("Encode text");std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);return out;
}
inline std::wstring wide(const std::string& text){
    if(text.empty())return {};const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n)win_error("Decode text");std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n);return out;
}
inline std::wstring uuid(){GUID id{};if(FAILED(CoCreateGuid(&id)))throw std::runtime_error("Create monitor ID");wchar_t value[40]{};StringFromGUID2(id,value,40);std::wstring result(value+1,36);std::transform(result.begin(),result.end(),result.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});return result;}
inline std::string exception_text(const std::exception& error){
    // MSVC's localized system_error::what() may be ACP rather than UTF-8.
    if(const auto* system=dynamic_cast<const std::system_error*>(&error))return "Win32 error "+std::to_string(system->code().value());
    return error.what();
}
inline std::wstring quote(const std::wstring& text){std::wstring out=L"\"";unsigned slashes=0;for(auto c:text){if(c==L'\\'){++slashes;continue;}out.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;out+=c;}out.append(slashes*2,L'\\');return out+L'"';}
inline fs::path executable(){wchar_t value[32768]{};if(!GetModuleFileNameW(nullptr,value,32768))win_error("Find monitor");return value;}
inline std::wstring user_sid(HANDLE process){
    HANDLE raw=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&raw))win_error("Read monitor owner");Handle token(raw);
    DWORD n=0;GetTokenInformation(raw,TokenUser,nullptr,0,&n);std::vector<BYTE> data(n);
    if(!GetTokenInformation(raw,TokenUser,data.data(),n,&n))win_error("Read monitor owner SID");
    LPWSTR text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text))win_error("Encode owner SID");
    std::wstring result(text);LocalFree(text);return result;
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor=nullptr;SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    explicit Security(const std::wstring& sddl){if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))win_error("Create monitor ACL");attributes.lpSecurityDescriptor=descriptor;}
    ~Security(){LocalFree(descriptor);}
};
inline bool elevated(){HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))win_error("Read elevation");Handle token(raw);TOKEN_ELEVATION value{};DWORD n=0;if(!GetTokenInformation(raw,TokenElevation,&value,sizeof(value),&n))win_error("Read elevation");return value.TokenIsElevated!=0;}
inline void save(const fs::path& path,const Json& json){
    auto temporary=path;temporary+=L".tmp";
    Security acl(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;BU)");
    Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,&acl.attributes,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)win_error("Create protected recovery record");
    if(GetFileAttributesW(temporary.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Recovery record is a reparse point");
    if(!SetFileSecurityW(temporary.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,acl.descriptor))win_error("Protect recovery record");
    const auto bytes=json.dump(2);DWORD written=0;
    if(!WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr) || written!=bytes.size() || !FlushFileBuffers(file.value))win_error("Write monitor recovery record");
    CloseHandle(file.value);file.value=nullptr;
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))win_error("Commit monitor recovery record");
}
inline Json read(const fs::path& path){std::ifstream file(path,std::ios::binary);Json json;file>>json;return json;}
inline fs::path protected_root(){
    wchar_t program_files[MAX_PATH]{};if(FAILED(SHGetFolderPathW(nullptr,CSIDL_PROGRAM_FILES,nullptr,SHGFP_TYPE_CURRENT,program_files)))throw std::runtime_error("Find Program Files");
    fs::path result(program_files);Security acl(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)");
    for(const auto* part:{L"FloraGPA",L"AutoDetect"}){
        result/=part;if(!CreateDirectoryW(result.c_str(),&acl.attributes) && GetLastError()!=ERROR_ALREADY_EXISTS)win_error("Create protected bootstrap directory");
        if(GetFileAttributesW(result.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)throw std::runtime_error("Bootstrap directory is a reparse point");
        if(!SetFileSecurityW(result.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,acl.descriptor))win_error("Protect bootstrap directory");
    }return result;
}
struct Registry {
    HKEY key=nullptr;
    Registry(){const auto e=RegOpenKeyExW(HKEY_LOCAL_MACHINE,registry_key,0,KEY_READ|KEY_WRITE|KEY_WOW64_64KEY,&key);if(e!=ERROR_SUCCESS){SetLastError(e);win_error("Open 64-bit AppInit configuration");}}
    ~Registry(){RegCloseKey(key);}
    Json get(const wchar_t* name){DWORD type=0,size=0;auto e=RegQueryValueExW(key,name,nullptr,&type,nullptr,&size);if(e==ERROR_FILE_NOT_FOUND)return {{"exists",false}};if(e!=ERROR_SUCCESS || size>65536)throw std::runtime_error("Read AppInit value");std::vector<BYTE> bytes(size);e=RegQueryValueExW(key,name,nullptr,&type,bytes.data(),&size);if(e!=ERROR_SUCCESS)throw std::runtime_error("Read AppInit value");return {{"exists",true},{"type",type},{"bytes",bytes}};}
    void put(const wchar_t* name,const Json& value){LSTATUS e;if(!value.at("exists").get<bool>())e=RegDeleteValueW(key,name);else{auto bytes=value.at("bytes").get<std::vector<BYTE>>();e=RegSetValueExW(key,name,0,value.at("type").get<DWORD>(),bytes.data(),static_cast<DWORD>(bytes.size()));}if(e!=ERROR_SUCCESS && e!=ERROR_FILE_NOT_FOUND){SetLastError(e);win_error("Write AppInit configuration");}RegFlushKey(key);}
};
inline Json string_value(const std::wstring& text){const auto* first=reinterpret_cast<const BYTE*>(text.c_str());return {{"exists",true},{"type",REG_SZ},{"bytes",std::vector<BYTE>(first,first+(text.size()+1)*sizeof(wchar_t))}};}
inline Json dword_value(DWORD value){const auto* first=reinterpret_cast<const BYTE*>(&value);return {{"exists",true},{"type",REG_DWORD},{"bytes",std::vector<BYTE>(first,first+sizeof(value))}};}
inline std::wstring string_data(const Json& value){if(!value.at("exists").get<bool>())return {};if(value.at("type")!=REG_SZ)throw std::runtime_error("Unsupported AppInit value type");auto bytes=value.at("bytes").get<std::vector<BYTE>>();if(bytes.size()%2 || bytes.size()<2 || bytes[bytes.size()-1] || bytes[bytes.size()-2])throw std::runtime_error("Malformed AppInit string");return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data()),bytes.size()/2-1);}
inline DWORD dword_data(const Json& value){if(!value.at("exists").get<bool>())return 0;auto bytes=value.at("bytes").get<std::vector<BYTE>>();if(value.at("type")!=REG_DWORD || bytes.size()!=4)throw std::runtime_error("Malformed AppInit DWORD");DWORD out=0;memcpy(&out,bytes.data(),4);return out;}
inline Json restore_appinit(const Json& log,const Json& current){
    if(current==log.at("installed"))return log.at("before");
    if(current.value("exists",false)){
        const auto text=string_data(current),remaining=remove_entry(text,wide(log.at("shim").get<std::string>()));
        if(text!=remaining)return string_value(remaining);
    }
    return current;
}
inline Json restore_load(const Json& log,const Json& current,const Json& load){
    return current==log.at("installed") && load==dword_value(1)?log.at("load_before"):load;
}
inline void recover(const fs::path& journal){
    if(!fs::exists(journal))return;auto log=read(journal);Registry registry;
    const auto current=registry.get(L"AppInit_DLLs"),restored=restore_appinit(log,current);
    if(current!=restored)registry.put(L"AppInit_DLLs",restored);
    const auto load=registry.get(L"LoadAppInit_DLLs"),restored_load=restore_load(log,current,load);
    if(load!=restored_load)registry.put(L"LoadAppInit_DLLs",restored_load);
    fs::remove(journal);
}
inline void preflight(){
    DWORD secure=0,size=sizeof(secure);const auto e=RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",L"UEFISecureBootEnabled",RRF_RT_REG_DWORD,nullptr,&secure,&size);
    if(e!=ERROR_SUCCESS && e!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Cannot determine Secure Boot state");
    if(secure)throw std::runtime_error("Auto-Detect is unavailable while Secure Boot is enabled");
    Registry registry;if(dword_data(registry.get(L"RequireSignedAppInit_DLLs")))throw std::runtime_error("AppInit requires signed DLLs; this build is unsigned");
}
inline fs::path stage(const fs::path& root){
    const auto path=root/uuid();Security acl(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)");
    if(!CreateDirectoryW(path.c_str(),&acl.attributes))win_error("Create bootstrap version directory");
    const auto source=executable().parent_path();
    for(const auto* name:{L"FloraCapture.exe",L"FloraCaptureShim.dll",L"FloraCaptureRuntime.dll"})fs::copy_file(source/name,path/name);
    for(const auto* name:{L"msvcp140.dll",L"vcruntime140.dll",L"vcruntime140_1.dll"}){
        const auto module=GetModuleHandleW(name);wchar_t dll[32768]{};
        if(module && GetModuleFileNameW(module,dll,32768))fs::copy_file(dll,path/name);
    }
    for(const auto& file:fs::directory_iterator(path))
        if(!SetFileSecurityW(file.path().c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,acl.descriptor))win_error("Protect staged binary");
    return path;
}
inline HANDLE spawn(const fs::path& path,const std::wstring& args,bool elevate){
    SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
    info.lpVerb=elevate?L"runas":L"open";info.lpFile=path.c_str();info.lpParameters=args.c_str();info.nShow=SW_HIDE;
    if(!ShellExecuteExW(&info))win_error("Start Auto-Detect manager");return info.hProcess;
}
inline Json event(const char* kind,bool ok=true){return {{"schema",1},{"event",kind},{"ok",ok}};}
}
