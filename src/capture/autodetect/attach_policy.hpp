#pragma once
#include <windows.h>
#include <string>
#include <algorithm>
#include <cstdint>

namespace flora::capture::autodetect {
inline std::wstring normalize_path(std::wstring path) {
    std::replace(path.begin(),path.end(),L'/',L'\\');
    if(path.starts_with(L"\\\\?\\"))path.erase(0,4);
    return path;
}
inline bool same_path(const std::wstring& a,const std::wstring& b) {
    const auto x=normalize_path(a),y=normalize_path(b);
    return !x.empty() && CompareStringOrdinal(x.c_str(),-1,y.c_str(),-1,TRUE)==CSTR_EQUAL;
}
struct AttachPolicy {
    static constexpr unsigned max_attempts=20;
    static constexpr ULONGLONG retry_ms=250, remote_ms=5000, graphics_ms=30000;
    unsigned attempts=0;
    ULONGLONG next=0;
    bool issued=false,finished=false;
    bool can_attempt(ULONGLONG now) const {return !issued && !finished && attempts<max_attempts && now>=next;}
    void attempted(ULONGLONG now){++attempts;next=now+retry_ms;}
};
}
