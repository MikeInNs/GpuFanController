#include "TemperatureForwarder.hpp"
#include <iostream>
using namespace std::chrono_literals;
int main(int argc,char** argv) try {
    if(argc!=2) return 2;
    using namespace fan;using Json=nlohmann::json;
    const auto start=SteadyClock::now();
    auto age=[&]{return std::chrono::duration<double>(SteadyClock::now()-start).count();};
    Json queries=Json::array(),trace=Json::array();
    bool blocked=false;double blockedEnd=0,shutdownStart=0;
    {
        TemperatureForwarder runtime(true,[&](const auto&) {return std::make_shared<SerialProbe>(argv[1]);},
            [](const auto&){},[](std::stop_token){},[&](const auto&) {
                const auto sampledAt=SteadyClock::now();
                queries.push_back(age());
                if(!blocked && age()>=4) {
                    blocked=true;std::this_thread::sleep_for(5s);blockedEnd=age();
                }
                return GpuReadings{{{"0000:01:00.0",500}},sampledAt,{}};
            },[](const auto&){return GpuReadings{};});
        runtime.configure({{"controllers",Json::array({{{"controllerId",std::string(32,'a')},
            {"groups",Json::array({{{"enabled",true},{"gpuPciAddress","0000:01:00.0"}},
                                   {{"enabled",false},{"gpuPciAddress",nullptr}}})}}})}});
        while(age()<12) {
            trace.push_back({{"at",age()},{"alerts",runtime.alerts()}});
            std::this_thread::sleep_for(100ms);
        }
        shutdownStart=age();
    }
    std::cout<<Json{{"queries",queries},{"trace",trace},{"blockedEnd",blockedEnd},
        {"shutdownSeconds",age()-shutdownStart}}.dump()<<'\n';
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
