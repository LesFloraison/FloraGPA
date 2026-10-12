#include "hud_model.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace flora::capture::hud {
const char* mode_name(Mode mode) noexcept {switch(mode){case Mode::Compact:return "compact";case Mode::Detailed:return "detailed";default:return "off";}}
Mode parse_mode(std::string_view text){
    if(text=="compact")return Mode::Compact;if(text=="detailed")return Mode::Detailed;if(text=="off")return Mode::Hidden;
    throw std::invalid_argument("HUD mode must be compact, detailed or off");
}
Hotkey parse_hotkey(std::string_view text){
    if(text=="off")return {};
    if(text.empty() || text.size()>64)throw std::invalid_argument("Invalid HUD hotkey");
    Hotkey result;
    for(size_t at=0;at<text.size();){
        const auto end=text.find('+',at);const auto part=text.substr(at,end==text.npos?text.size()-at:end-at);
        if(result.key)throw std::invalid_argument("Hotkey must end with one letter, digit or F1-F24");
        if(part=="Ctrl" && !result.control)result.control=true;
        else if(part=="Shift" && !result.shift)result.shift=true;
        else if(part=="Alt" && !result.alt)result.alt=true;
        else if(part.size()==1 && ((part[0]>='A' && part[0]<='Z') || (part[0]>='0' && part[0]<='9')))result.key=uint32_t(part[0]);
        else if(part.size()>=2 && part[0]=='F'){
            unsigned number=0;const auto parsed=std::from_chars(part.data()+1,part.data()+part.size(),number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=part.data()+part.size() || number<1 || number>24 || part[1]=='0')throw std::invalid_argument("Invalid function key");
            result.key=0x70+number-1;
        }else throw std::invalid_argument("Invalid or repeated hotkey component");
        if(end==text.npos)break;at=end+1;if(at==text.size())throw std::invalid_argument("Incomplete hotkey");
    }
    if(!result.key || (!result.control && !result.shift && !result.alt))throw std::invalid_argument("Hotkey requires a modifier and a key, or off");
    return result;
}
std::string hotkey_name(const Hotkey& key){
    if(!key.key)return "off";std::string result;
    if(key.control)result+="Ctrl+";if(key.shift)result+="Shift+";if(key.alt)result+="Alt+";
    if(key.key>=0x70 && key.key<=0x87)result+="F"+std::to_string(key.key-0x70+1);
    else result+=char(key.key);return result;
}
void Settings::validate() const {
    if(mode!=Mode::Compact && mode!=Mode::Detailed && mode!=Mode::Hidden)throw std::invalid_argument("Invalid HUD mode");
    if(parse_hotkey(hotkey_name(mode_key))!=mode_key || parse_hotkey(hotkey_name(capture_key))!=capture_key)throw std::invalid_argument("Invalid hotkey state");
    if(mode_key.key && mode_key==capture_key)throw std::invalid_argument("HUD and capture hotkeys must differ");
}
bool KeyEdge::update(const Hotkey& key,const std::array<bool,256>& keys,bool foreground) noexcept {
    const bool down=key.key && key.key<keys.size() && keys[key.key] && keys[0x11]==key.control && keys[0x10]==key.shift && keys[0x12]==key.alt;
    const bool trigger=foreground && focused_ && down && !down_;
    down_=down;focused_=foreground;return trigger;
}
void FrameClock::present(uint64_t now,bool successful,bool test){
    if(!successful || test)return;
    ++fps_.frames;
    if(!start_ || now<*start_){start_=now;count_=0;history_.clear();fps_.current.reset();fps_.minimum.reset();fps_.maximum.reset();return;}
    ++count_;const auto elapsed=now-*start_;
    if(elapsed<1000000000ull)return;
    const double rate=double(count_)*1e9/double(elapsed);history_.emplace_back(now,rate);
    while(!history_.empty() && now-history_.front().first>=7000000000ull)history_.pop_front();
    fps_.current=rate;fps_.minimum=rate;fps_.maximum=rate;
    for(const auto& sample:history_){fps_.minimum=(std::min)(*fps_.minimum,sample.second);fps_.maximum=(std::max)(*fps_.maximum,sample.second);}
    start_=now;count_=0;
}
Fps FrameClock::value(uint64_t now) const {
    auto result=fps_;
    if(!start_ || now<*start_ || now-*start_>=2000000000ull){result.current.reset();result.minimum.reset();result.maximum.reset();}
    return result;
}
void Message::update(std::string identity,std::string text,uint64_t now,uint64_t duration){
    if(identity_==identity)return;
    identity_=std::move(identity);text_=std::move(text);persistent_=duration==0;expiry_=now+duration;
}
std::string Message::value(uint64_t now) const {return persistent_ || now<expiry_?text_:std::string{};}
}
