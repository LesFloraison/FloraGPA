#include "auto_detect.hpp"
#include "../autodetect/registry_model.hpp"
#include "../autodetect/monitor_support.hpp"
#include "../autodetect/attach_policy.hpp"
#include <thread>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace flora::capture;
using namespace flora::capture::autodetect;
int main(){try{
    auto check=[](bool value){if(!value)throw std::runtime_error("Auto-Detect policy check failed");};
    check(remove_entry(L"a.dll C:\\FLORA\\shim.dll,b.dll",L"c:\\flora\\SHIM.dll")==L"a.dll ,b.dll");
    check(remove_entry(L"C:\\FLORA\\shim.dll.backup",L"C:\\FLORA\\shim.dll")==L"C:\\FLORA\\shim.dll.backup");
    check(remove_entry(L"a.dll\tshim.dll  b.dll,shim.dll",L"shim.dll")==L"a.dll\t  b.dll,");
    check(remove_entry(L"",L"shim.dll").empty());
    check(string_data(string_value(L"existing.dll, C:\\Path\\test.dll"))==L"existing.dll, C:\\Path\\test.dll");
    check(dword_data(dword_value(1))==1);check(dword_data(Json{{"exists",false}})==0);
    const Json absent={{"exists",false}};
    const Json journal={{"shim","C:\\Flora\\shim.dll"},{"before",string_value(L"prior.dll")},
        {"installed",string_value(L"prior.dll C:\\Flora\\shim.dll")},{"load_before",absent}};
    check(restore_appinit(journal,journal.at("installed"))==journal.at("before"));
    const auto third_party=string_value(L"prior.dll C:\\Flora\\shim.dll,new.dll");
    check(restore_appinit(journal,third_party)==string_value(L"prior.dll ,new.dll"));
    check(restore_load(journal,third_party,dword_value(1))==dword_value(1));
    check(restore_load(journal,journal.at("installed"),dword_value(1))==absent);
    check(restore_load(journal,journal.at("installed"),dword_value(0))==dword_value(0));
    check(restore_appinit(journal,absent)==absent);
    check(restore_appinit(journal,string_value(L"replacement.dll"))==string_value(L"replacement.dll"));
    bool rejected=false;try{string_data(dword_value(1));}catch(...){rejected=true;}check(rejected);
    check(excluded(L"C:\\Apps\\Steam.exe"));check(excluded(L"C:\\Apps\\steamwebhelper.exe"));
    check(excluded(L"C:\\Apps\\FloraGPA.exe"));check(excluded(L"C:\\Apps\\UnityCrashHandler64.exe"));
    check(excluded(L"C:\\Tools\\codex-computer-use.exe"));
    check(!excluded(L"D:\\Games\\UnlistedGame.exe"));
    check(!excluded(L"D:\\Games\\SteamWorld.exe"));check(!excluded(L"D:\\Games\\Flora.exe"));
    check(created(GetCurrentProcess())!=0);check(!created(nullptr));
    check(same_path(L"D:/Games/Endfield.exe",L"d:\\games\\ENDFIELD.exe"));
    check(same_path(L"\\\\?\\D:\\Games\\Endfield.exe",L"D:\\Games\\Endfield.exe"));
    check(!same_path(L"D:\\Games\\Endfield.exe",L"E:\\Other\\Endfield.exe"));
    check(!same_path(L"",L""));
    AttachPolicy policy;
    for(unsigned i=0;i<20;++i){const auto now=1000+i*250;check(policy.can_attempt(now));policy.attempted(now);check(!policy.can_attempt(now+249));}
    check(!policy.can_attempt(100000));
    policy=AttachPolicy{};policy.issued=true;check(!policy.can_attempt(100000));
    policy=AttachPolicy{};policy.finished=true;check(!policy.can_attempt(100000));
    Progress progress;std::array<AttachProgress*,16> claims{};std::array<std::thread,16> threads;
    for(unsigned i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{claims[i]=claim_progress(progress,42,100);});
    for(auto& thread:threads)thread.join();
    for(auto* claim:claims)check(claim && claim==claims[0]);
    check(claim_progress(progress,42,101)!=claims[0]); // Reused PID is a different instance.
    check(wide(utf8(L"D:\\游戏\\Capture"))==L"D:\\游戏\\Capture");
    std::cout<<"Auto-Detect registry merge, exclusion, identity and Unicode policy passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
