#include "AlertCenter.hpp"
#include <atomic>
#include <iostream>
using Json=nlohmann::json;
using namespace std::chrono_literals;
void check(bool ok) {if(!ok) throw std::runtime_error("Alert assertion failed");}
Json event(unsigned type,const std::vector<std::uint8_t>& payload) {
    return {{"events",Json::array({{{"type",type},{"payload",payload},{"receivedAtMs",1000}}})},{"dropped",0}};
}
std::vector<std::uint8_t> alert(unsigned raised,unsigned cleared,unsigned active) {
    std::vector<std::uint8_t> p(18);
    for(auto [i,v]:{std::pair{6,raised},{8,cleared},{10,active}}) {p[i]=v&255;p[i+1]=v>>8;}
    return p;
}
template<class F> void wait(F f) {
    const auto until=std::chrono::steady_clock::now()+2s;
    while(!f() && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(5ms);
    check(f());
}
int main() try {
    fan::AlertCenter center;
    Json config{{"controllers",Json::array({{{"controllerId","a"}},{{"controllerId","b"}}})}};
    center.configure(config,true);center.connection("a",true);
    center.ingest("a",event(0x87,alert(8,0,8)));
    check(center.status()["criticalActive"]==true);
    check(center.status()["history"].size()==1);
    center.ingest("a",event(0x87,alert(8,0,8))); // Duplicate: no repeated raise.
    check(center.status()["history"].size()==1);
    center.ingest("a",event(0x87,alert(0,8,0)));
    check(center.status()["history"].size()==2 && center.status()["criticalActive"]==false);
    center.ingest("a",event(0x87,alert(8,8,8))); // Inconsistent masks ignored.
    check(center.status()["history"].size()==2);
    center.ingest("a",event(0x87,alert(0x8000,0,0x8000)));
    check(center.status()["active"][0]["severity"]=="warning");
    center.connection("b",false);check(center.status()["criticalActive"]==true);
    center.connection("b",false);check(center.status()["history"].size()==4);
    center.connection("b",true);check(center.status()["criticalActive"]==false);
    std::vector<std::uint8_t> hb(21);hb[4]=4;hb[20]=1;
    center.ingest("a",event(0x83,hb));check(center.status()["criticalActive"]==true);
    center.connection("a",false);
    check(center.status()["controllers"][0]["stale"]==true);
    check(center.status()["controllers"][0]["activeMasks"][0]==4); // Not fabricated clear on disconnect.
    center.connection("a",true);hb[4]=0;
    center.ingest("a",event(0x83,hb));check(center.status()["criticalActive"]==false);
    std::vector<std::uint8_t> status(58);status[8]=8;status[34]=16;
    center.ingest("a",event(0x84,status));check(center.status()["controllers"][0]["lastStatusLatchedMasks"][0]==8);
    check(center.status()["active"][0]["message"]=="Fan 2 slow/stopped");
    for(int i=0;i<280;++i) center.ingest("b",event(0x87,alert(i%2?0:2,i%2?2:0,i%2?0:2)));
    check(center.status()["history"].size()==256 && center.status()["historyTruncated"]==true);
    center.ingest("b",{{"dropped",1},{"events",Json::array()}});
    check(center.status()["criticalActive"]==true);
    config["controllers"]=Json::array();center.configure(config,true);
    check(center.status()["active"].empty() && center.status()["criticalActive"]==false);
    check(center.status()["beeper"]["attempts"]==0);

    std::atomic<int> tones=0;
    fan::AudibleAlarm sound([&]{++tones;},200ms);
    sound.update(true,true);std::this_thread::sleep_for(30ms);check(tones==0);
    sound.enable(true);wait([&]{return tones==1;});
    for(int i=0;i<10;++i) sound.update(true,true);
    std::this_thread::sleep_for(40ms);check(tones==1); // Global rate limit includes new critical faults.
    wait([&]{return tones>=2;});sound.enable(false);
    const int quiet=tones;std::this_thread::sleep_for(250ms);check(tones==quiet);
    sound.update(false,false);sound.enable(true);sound.update(false,true);
    wait([&]{return tones>quiet;}); // Raised/cleared in one receive batch still sounds once.
    sound.enable(false);
    fan::AudibleAlarm unavailable([]{throw std::runtime_error("Simulated permission denied");},200ms);
    unavailable.enable(true);unavailable.update(true,true);
    wait([&]{return unavailable.status()["state"]=="unavailable";});
    check(unavailable.status()["error"]=="Simulated permission denied");
    unavailable.enable(false);
    std::cout<<"Alert transitions/reconciliation/history/isolation and fake beeper policy passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
