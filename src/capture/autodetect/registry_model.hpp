#pragma once
#include <string>
#include <cwctype>

namespace flora::capture::autodetect {
inline std::wstring remove_entry(const std::wstring& text,const std::wstring& own) {
    // Preserve all bytes outside our token, including third-party separators.
    auto result=text;
    for(std::size_t i=0;i<result.size();){
        if(result[i]==L',' || iswspace(result[i])){++i;continue;}
        auto end=i;while(end<result.size() && result[end]!=L',' && !iswspace(result[end]))++end;
        const auto token=result.substr(i,end-i);
        if(_wcsicmp(token.c_str(),own.c_str())==0)result.erase(i,end-i);else i=end;
    }
    return result;
}
}
