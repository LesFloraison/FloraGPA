#include "hud_model.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace flora::capture::hud;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F f){try{f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("Invalid settings accepted");}
int main(){try{
    Settings settings;settings.validate();
    for(const auto text:{"off","Ctrl+F1","Ctrl+Shift+C","Alt+F24","Shift+7"})require(hotkey_name(parse_hotkey(text))==text,"Hotkey round trip");
    for(const auto text:{"","Ctrl","Ctrl+","Ctrl+Ctrl+C","C","Ctrl+c","Win+C","Ctrl+F0","Ctrl+F25","Ctrl+F01","Ctrl+C+F1","Ctrl++C"})rejects([&]{parse_hotkey(text);});
    settings.capture_key=settings.mode_key;rejects([&]{settings.validate();});
    settings.mode_key={};settings.capture_key={};settings.validate();
    require(failure_summary("ID3D11DeviceContext::ClearRenderTargetView: MSAA sample-preserving capture is not implemented")=="Capture failed: MSAA","MSAA summary hides cause");
    require(failure_summary("API: Device removed")=="Capture failed: Device removed" && failure_summary("")=="Capture failed","Failure summary");
    require(parse_mode("off")==Mode::Hidden,"Hidden mode");rejects([]{parse_mode("unknown");});
    std::array<bool,256> keys{};KeyEdge edge;const auto capture=parse_hotkey("Ctrl+Shift+C");
    require(!edge.update(capture,keys,true),"Initial focus");keys[0x11]=keys[0x10]=keys['C']=true;
    require(edge.update(capture,keys,true),"Pressed chord");require(!edge.update(capture,keys,true),"Key repeat");
    require(!edge.update(capture,keys,false) && !edge.update(capture,keys,true),"Focus with held key");
    keys['C']=false;edge.update(capture,keys,true);keys['C']=true;require(edge.update(capture,keys,true),"Release and re-press");
    keys[0x12]=true;require(!edge.update(capture,keys,true),"Extra modifier");
    keys[0x12]=false;keys[0x5b]=true;require(!edge.update(capture,keys,true),"Windows modifier was ignored");
    FrameClock clock;require(!clock.value(0).current,"No fabricated FPS");clock.present(0,true,false);
    {
        Settings raw_settings;RawKeyboard raw;std::array<bool,256> empty{};raw.seed(empty,true,raw_settings);
        raw.event(0xa2,true,true,raw_settings);raw.event(0xa0,true,true,raw_settings);
        require(raw.event('C',true,true,raw_settings).capture,"Raw short chord missed");
        for(unsigned repeat=0;repeat<1000;++repeat)require(!raw.event('C',true,true,raw_settings).capture,"Raw repeat triggered capture");
        raw.event('C',false,true,raw_settings);raw.event(0xa1,true,true,raw_settings);raw.event(0xa0,false,true,raw_settings);
        require(raw.event('C',true,true,raw_settings).capture,"Releasing left shift cleared held right shift");
        raw.event('C',false,false,raw_settings);require(!raw.event('C',true,false,raw_settings).capture,"Background raw input triggered");
        require(!raw.event('C',true,true,raw_settings).capture,"Focus with held raw key triggered");raw.event('C',false,true,raw_settings);
        require(raw.event('C',true,true,raw_settings).capture,"Raw re-press after focus failed");raw.event('C',false,true,raw_settings);raw.event(0xa1,false,true,raw_settings);
        require(raw.event(0x70,true,true,raw_settings).mode,"Raw mode chord failed");require(!raw.event(0x70,true,true,raw_settings).mode,"Raw mode repeated");raw.event(0x70,false,true,raw_settings);
        raw.event(0x5b,true,true,raw_settings);require(!raw.event(0x70,true,true,raw_settings).mode,"Raw Windows modifier ignored");
        require(!raw.event(255,true,true,raw_settings).capture && !raw.event(512,true,true,raw_settings).mode,"Invalid raw key accepted");
        empty[0xa2]=empty[0xa0]=empty['C']=true;raw.seed(empty,true,raw_settings);
        require(!raw.event('C',true,true,raw_settings).capture,"Held chord triggered after focus resynchronization");raw.event('C',false,true,raw_settings);require(raw.event('C',true,true,raw_settings).capture,"Fresh chord blocked after resynchronization");
    }
    for(uint64_t i=1;i<=60;++i)clock.present(i*1000000000ull/60,true,false);
    auto fps=clock.value(1000000000);require(fps.frames==61 && fps.current && std::abs(*fps.current-60)<0.0001,"60 FPS window");
    clock.present(2000000000,false,false);clock.present(2000000000,true,true);require(clock.value(2000000000).frames==61,"TEST/failure counted");
    require(!clock.value(3000000000).current,"Stale FPS retained");
    for(uint64_t second=2;second<=9;++second)clock.present(second*1000000000ull,true,false);
    fps=clock.value(9000000000);require(*fps.minimum==1 && *fps.maximum==1,"Seven-second rolling window");
    Message message;message.update("file1","Saved",100,4000);message.update("file1","Saved",4000,4000);
    require(message.value(4099)=="Saved" && message.value(4100).empty(),"Polling extended message lifetime");
    message.update("working","Capturing",5000,0);require(message.value(999999)=="Capturing","Persistent progress");
    message.update("failure","Capture failed",10000,8000);require(message.value(18000).empty(),"Error timeout");
    std::cout<<"HUD settings, focus edges, measured FPS and message lifetimes passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
