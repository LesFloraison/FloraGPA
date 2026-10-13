#include "raw_input.hpp"
#include "hud_model.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace flora::capture::hud;
void require(bool good,const char* why){if(!good)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("Malformed raw batch accepted");}
int main(){try{
    // Exercise both allowed DWORD starts, variable-size non-keyboard records,
    // native pointer alignment and an immutable source allocation.
    for(size_t displacement:{size_t(0),size_t(4)}){
        std::vector<uint64_t> storage(128,0xa5a5a5a5a5a5a5a5ull);auto* data=reinterpret_cast<unsigned char*>(storage.data())+displacement;size_t used=0;uint32_t count=0;
        const auto append=[&](DWORD type,const void* payload,size_t size){
            if(count)used+=(0-(reinterpret_cast<uintptr_t>(data)+used))&(sizeof(UINT_PTR)-1);
            RAWINPUTHEADER header{};header.dwType=type;header.dwSize=DWORD(sizeof header+size);memcpy(data+used,&header,sizeof header);if(size)memcpy(data+used+sizeof header,payload,size);used+=header.dwSize;++count;
        };
        const uint8_t hid[]{1,2,3,4,5};append(RIM_TYPEHID,hid,sizeof hid);
        RAWKEYBOARD key{};key.VKey=VK_LCONTROL;append(RIM_TYPEKEYBOARD,&key,sizeof key);key.VKey=VK_LSHIFT;append(RIM_TYPEKEYBOARD,&key,sizeof key);key.VKey='C';append(RIM_TYPEKEYBOARD,&key,sizeof key);
        RAWMOUSE mouse{};append(RIM_TYPEMOUSE,&mouse,sizeof mouse);append(RIM_TYPEKEYBOARD,&key,sizeof key);key.Flags=RI_KEY_BREAK;append(RIM_TYPEKEYBOARD,&key,sizeof key);
        const auto untouched=storage;const auto keys=keyboard_batch(data,used,count);require(keys.size()==5,"Non-keyboard record handling");require(storage==untouched,"Raw batch mutated application data");
        RawKeyboard keyboard;Settings settings;std::array<bool,256> empty{};keyboard.seed(empty,true,settings);unsigned captures=0;
        for(const auto& item:keys)captures+=keyboard.event(item.VKey,!(item.Flags&RI_KEY_BREAK),true,settings).capture;
        require(captures==1,"Repeated raw batch make triggered twice");
        for(size_t size=0;size<used;++size)rejects([&]{keyboard_batch(data,size,count);});
        rejects([&]{keyboard_batch(data,used,count+1);});
        RAWINPUTHEADER header{};memcpy(&header,data,sizeof header);const auto original=header.dwSize;
        for(DWORD size:{DWORD(0),DWORD(sizeof(RAWINPUTHEADER)-1),DWORD(-1)}){header.dwSize=size;memcpy(data,&header,sizeof header);rejects([&]{keyboard_batch(data,used,count);});}header.dwSize=original;
        header.dwType=RIM_TYPEKEYBOARD;memcpy(data,&header,sizeof header);rejects([&]{keyboard_batch(data,used,count);});
    }
    require(keyboard_batch(nullptr,0,0).empty(),"Empty batch rejected");rejects([]{keyboard_batch(nullptr,64,1);});
    std::cout<<"Native x64 raw input alignment, bounds, immutable records and repeated make edges passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
