#include "raw_input.hpp"
#include <cstring>
#include <stdexcept>
namespace flora::capture::hud {
std::vector<RAWKEYBOARD> keyboard_batch(const void* data,size_t capacity,uint32_t records){
    static_assert(sizeof(void*)==8);
    if(!records)return {};
    if(!data || records>capacity/sizeof(RAWINPUTHEADER))throw std::invalid_argument("Invalid raw input batch bounds");
    const auto* bytes=static_cast<const unsigned char*>(data);size_t offset=0;
    std::vector<RAWKEYBOARD> keys;
    for(uint32_t i=0;i<records;++i){
        if(capacity-offset<sizeof(RAWINPUTHEADER))throw std::invalid_argument("Truncated raw input header");
        RAWINPUTHEADER header{};memcpy(&header,bytes+offset,sizeof header);
        if(header.dwSize<sizeof header || header.dwSize>capacity-offset)throw std::invalid_argument("Invalid raw input record size");
        if(header.dwType==RIM_TYPEKEYBOARD){
            if(header.dwSize<offsetof(RAWINPUT,data)+sizeof(RAWKEYBOARD))throw std::invalid_argument("Truncated raw keyboard record");
            RAWKEYBOARD key{};memcpy(&key,bytes+offset+offsetof(RAWINPUT,data),sizeof key);keys.push_back(key);
        }
        offset+=header.dwSize;
        if(i+1<records){
            // Equivalent to the native x64 NEXTRAWINPUTBLOCK alignment, without
            // forming an unchecked pointer beyond the supplied allocation.
            const size_t padding=(0-(reinterpret_cast<uintptr_t>(data)+offset))&(sizeof(UINT_PTR)-1);
            if(padding>capacity-offset)throw std::invalid_argument("Truncated raw input alignment");
            offset+=padding;
        }
    }
    return keys;
}
}
