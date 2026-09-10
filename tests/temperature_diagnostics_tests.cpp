#include "TemperatureDiagnostics.hpp"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void check(bool value) {if(!value) throw std::runtime_error("Temperature diagnostic assertion failed");}
int main() try {
    using namespace fan;using Json=nlohmann::json;
    const auto now=SteadyClock::time_point{}+100s;
    TemperatureDiagnostics diagnostics;
    std::array<GpuReadings,2> readings{{{{{"0000:01:00.0",500}},now,{}},{}}};
    std::array<GpuQueryTiming,2> timings{{{now,20ms,false},{}}};
    Json groups=Json::array({{{"enabled",true},{"gpuPciAddress","0000:01:00.0"}},{{"enabled",false},{"gpuPciAddress",nullptr}}});
    auto packet=[&](SteadyClock::time_point at,bool reply=true) {
        diagnostics.packet("controller-a",groups,mappedTemperatures(groups,readings,at),readings,timings,at,123456,reply);
    };
    diagnostics.queryCompleted(0,readings[0],timings[0],now+20ms);
    check(diagnostics.history().empty());
    packet(now);packet(now+1s);check(diagnostics.history().size()==1);
    check(diagnostics.history()[0]["group"]==1);
    timings[0]={now+1s,20ms,true};
    packet(now+3s,false);packet(now+4s);
    auto event=diagnostics.history().back();
    check(diagnostics.history().size()==2 && event["reason"]=="sample_expired");
    check(event["sources"][0]["sampleAgeMs"]==3000 && event["sources"][0]["queryInFlightAgeMs"]==2000);
    check(event["sources"][0]["queryDurationMs"]==20 && event["sources"][0]["error"].is_null());
    check(event["sources"][1]["queryDurationMs"].is_null() && event["replyReceived"]==false);
    readings[0]={{},now+1s,"GPU query timed out: driver error"};timings[0]={now+1s,3s,false};
    diagnostics.queryCompleted(0,readings[0],timings[0],now+4s);
    diagnostics.queryCompleted(0,readings[0],timings[0],now+5s);
    check(diagnostics.history().size()==3 && diagnostics.history().back()["transition"]=="error");
    readings[0]={{{"0000:01:00.0",500}},now+5s,{}};timings[0]={now+5s,25ms,false};
    diagnostics.queryCompleted(0,readings[0],timings[0],now+5s+25ms);packet(now+5s+25ms);
    auto history=diagnostics.history();
    check(history.size()==5 && history.back()["transition"]=="recovered");
    check(history[3]["previousError"]=="GPU query timed out: driver error");
    // Another controller is independent. Ambiguous providers stay invalid.
    readings[1]=readings[0];
    diagnostics.packet("controller-b",groups,mappedTemperatures(groups,readings,now+5s),readings,timings,now+5s,123457,true);
    check(diagnostics.history().back()["reason"]=="duplicate_readings");
    groups[0]["enabled"]=false;packet(now+5s);
    check(diagnostics.history().back()["transition"]=="disabled");
    diagnostics.forget("controller-a");packet(now+5s);check(diagnostics.history().size()==7);
    for(int i=0;i<300;++i) {
        readings[1].error="AMD sensor error "+std::to_string(i);
        diagnostics.queryCompleted(1,readings[1],timings[1],now+5s);
    }
    check(diagnostics.history().size()==256);
    std::cout<<"Validity/query transitions, expiry, recovery, in-flight timing, deduplication and bounded history passed\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
