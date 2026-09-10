#include "CalibrationControl.hpp"
#include "CalibrationWarning.hpp"
#include <iostream>
#include <stdexcept>
using Json=nlohmann::json;
template<class F> void rejects(F f) {try{f();}catch(const std::exception&){return;}throw std::runtime_error("Expected calibration refusal");}
int main() {
    using namespace fan::calibration;
    Json request{{"confirmed",true},{"configGeneration",1},{"calibrationGeneration",7}};
    validateRequest(request,true);validateRequest({{"confirmed",true}},false);
    for(const auto& value:Json::array({false,1,"yes",nullptr})) {
        auto bad=request;bad["confirmed"]=value;rejects([&]{validateRequest(bad,true);});
    }
    for(const auto& value:Json::array({-1,1.5,4294967296ULL,"7",nullptr})) {
        auto bad=request;bad["calibrationGeneration"]=value;rejects([&]{validateRequest(bad,true);});
    }
    rejects([&]{validateRequest({{"confirmed",true},{"other",0}},false);});
    const std::string id(32,'a');
    Json runtime{{"enabled",true},{"controllers",Json::array({{{"controllerId",id},{"responsive",true},
        {"lastReplyAgeMs",5000},{"requiredValidMask",1},{"validMask",1}}})}};
    requireForwarding(runtime,id,0);
    rejects([&]{requireForwarding(runtime,id,1);});
    rejects([&]{requireForwarding(runtime,std::string(32,'b'),0);});
    for(auto key:{"requiredValidMask","validMask"}) {
        auto bad=runtime;bad["controllers"][0][key]=0;rejects([&]{requireForwarding(bad,id,0);});
    }
    for(const auto& age:Json::array({nullptr,6500,10000})) {
        auto bad=runtime;bad["controllers"][0]["lastReplyAgeMs"]=age;rejects([&]{requireForwarding(bad,id,0);});
    }
    auto bad=runtime;bad["enabled"]=false;rejects([&]{requireForwarding(bad,id,0);});
    bad=runtime;bad["controllers"][0]["responsive"]=false;rejects([&]{requireForwarding(bad,id,0);});
    Json config{{"voltageWarningLowMv",11000},{"voltageHighMv",13200},
        {"groups",Json::array({{{"enabled",true},{"expectedFanMask",3},{"warningTemperatureDeciC",750}}})}};
    Json state{{"calibrationActive",false},{"inaPresent",true},{"activeFaults",0},{"hostUpdateAgeMs",5000},
        {"groups",Json::array({{{"temperatureDeciC",500},{"voltageMv",12000}}})}};
    requireSafeStart(config,state,0);
    for(int mask:{1,2,4,8,64}) {bad=state;bad["activeFaults"]=mask;rejects([&]{requireSafeStart(config,bad,0);});}
    for(const auto& temp:Json::array({nullptr,750,850})) {
        bad=state;bad["groups"][0]["temperatureDeciC"]=temp;rejects([&]{requireSafeStart(config,bad,0);});
    }
    for(const auto& volts:Json::array({nullptr,0,10999,13201})) {
        bad=state;bad["groups"][0]["voltageMv"]=volts;rejects([&]{requireSafeStart(config,bad,0);});
    }
    bad=state;bad["calibrationActive"]=true;rejects([&]{requireSafeStart(config,bad,0);});
    bad=state;bad["hostUpdateAgeMs"]=6500;rejects([&]{requireSafeStart(config,bad,0);});
    bad=config;bad["groups"][0]["enabled"]=false;rejects([&]{requireSafeStart(bad,state,0);});
    auto overrides=Json::array({"power_voltage_low"});
    auto overrideRequest=request;overrideRequest["overrideWarnings"]=overrides;validateRequest(overrideRequest,true);
    for(const auto& invalid:Json::array({true,"all",Json::array({"temperature_high"}),Json::array({"power_voltage_low","power_voltage_low"})})) {
        overrideRequest["overrideWarnings"]=invalid;rejects([&]{validateRequest(overrideRequest,true);});
    }
    bad=state;bad["groups"][0]["voltageMv"]=10000;
    try {requireSafeStart(config,bad,0);throw std::runtime_error("Expected voltage warning");}
    catch(const SafetyRefusal& e) {
        const auto warning=fan::calibrationWarning(e.details);
        if(!warning.canOverride || warning.overrides!=overrides || warning.message.find("overheat")==std::string::npos)
            throw std::runtime_error("Risk warning presentation invalid");
    }
    requireSafeStart(config,bad,0,overrides);
    rejects([&]{requireSafeStart(config,bad,0);}); // No persisted override.
    bad["groups"][0]["voltageMv"]=14000;rejects([&]{requireSafeStart(config,bad,0,overrides);}); // New warning not acknowledged.
    const auto all=Json::array({"power_voltage_low","power_voltage_high","power_voltage_unavailable"});
    requireSafeStart(config,bad,0,all);
    bad["groups"][0]["voltageMv"]=nullptr;requireSafeStart(config,bad,0,all);
    bad["groups"][0]["temperatureDeciC"]=750;
    try {requireSafeStart(config,bad,0,all);throw std::runtime_error("Temperature must stay mandatory");}
    catch(const SafetyRefusal& e) {if(fan::calibrationWarning(e.details).canOverride) throw std::runtime_error("Unsafe override offered");}
    for(int faults:{1,64}) {bad=state;bad["activeFaults"]=faults;rejects([&]{requireSafeStart(config,bad,0,all);});}
    for(int faults:{2,4,8}) {bad=state;bad["activeFaults"]=faults;requireSafeStart(config,bad,0,all);}
    bad=state;bad["calibrationActive"]=true;rejects([&]{requireSafeStart(config,bad,0,all);});
    bad=state;bad["groups"][0]["temperatureDeciC"]=nullptr;rejects([&]{requireSafeStart(config,bad,0,all);});
    std::cout<<"Calibration confirmation, freshness, mapping and power/temperature admission checks passed\n";
}
