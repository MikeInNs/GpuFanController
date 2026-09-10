#include "SerialProbe.hpp"
#include "CalibrationControl.hpp"
#include <iostream>
#include <thread>
#include <future>
#include <atomic>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    try {
        fan::SerialProbe probe(argv[1]);
        auto hello=probe.hello();
        if(std::string(argv[2])=="adapter-names") {
            auto names=probe.snapshot().at("configuration").at("adapterNames");
            names["groups"][0]={{"pciAddress","0000:01:00.0"},{"name","Tesla V100"}};
            std::cout<<probe.updateAdapterNames({{"confirmed",true},{"adapterNames",names}}).dump()<<'\n';return 0;
        }
        if(std::string(argv[2])=="result") {std::cout<<probe.snapshot().dump()<<'\n';return 0;}
        if(std::string(argv[2]).starts_with("settings-")) {
            const auto action=std::string(argv[2]).substr(9);
            nlohmann::json result;
            if(action=="supply") result=probe.updateSupply({{"confirmed",true},{"configGeneration",1},{"changes",{{"voltageHighMv",13500}}}});
            else if(action=="pwm") result=probe.updateGroup(1,{{"confirmed",true},{"configGeneration",1},{"calibrationGeneration",7},
                {"changes",{{"minimumDutyPercent",40},{"startupDutyPercent",70},{"startupTimeMs",2000}}}});
            else result=probe.clearLatched({{"confirmed",true},{"scope",action}});
            std::cout<<result.dump()<<'\n';return 0;
        }
        if(std::string(argv[2])=="live") {
            std::atomic<bool> sending=false;
            auto temperature=std::async(std::launch::async,[&]{
                std::chrono::steady_clock::time_point sent;
                probe.temperatures([&]{sending=true;return fan::protocol::TemperatureSnapshot{};},sent);
            });
            while(!sending) std::this_thread::yield(); // Callback runs with the serial slot held.
            const auto began=std::chrono::steady_clock::now();
            bool refused=false;
            try {probe.liveStatus();} catch(const fan::ConfigConflict&) {refused=true;}
            if(!refused || std::chrono::steady_clock::now()-began>std::chrono::milliseconds(200))
                throw std::runtime_error("Live polling waited behind a temperature transaction");
            temperature.get();
            std::cout<<probe.liveStatus().dump()<<'\n';return 0;
        }
        if(std::string(argv[2]).starts_with("group-")) {
            const std::string action=std::string(argv[2]).substr(6);
            if(action=="edit") {
                std::cout<<probe.updateGroup(1,{{"confirmed",true},{"configGeneration",1},{"calibrationGeneration",7},
                    {"changes",{{"enabled",false},{"expectedFanMask",2}}}}).dump()<<'\n';
            } else {
                std::cout<<probe.setGroupMode(1,{{"confirmed",true},{"mode",action},{"timeoutSeconds",action=="auto"?0:300},{"configGeneration",1}}).dump()<<'\n';
            }
            return 0;
        }
        if(std::string(argv[2])=="events") {
            probe.status();
            std::chrono::steady_clock::time_point sent;
            probe.temperatures([]{return fan::protocol::TemperatureSnapshot{};},sent);
            const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(700);
            while(std::chrono::steady_clock::now()<until) {probe.receiveEvents();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            std::cout<<probe.takeEvents().dump()<<'\n';return 0;
        }
        if(std::string(argv[2]).starts_with("calibration")) {
            const int group=std::string(argv[2])=="calibration1"?1:0;
            const bool overridePower=std::string(argv[2]).find("override")!=std::string::npos;
            nlohmann::json request{{"confirmed",true},{"configGeneration",1},{"calibrationGeneration",7}};
            if(overridePower) request["overrideWarnings"]={"power_voltage_low"};
            const auto started=probe.startCalibration(group,request,[]{});
            const auto progress=probe.status();
            const auto aborted=probe.abortCalibration();
            const auto after=probe.snapshot();
            if(overridePower) {
                request.erase("overrideWarnings");
                bool refused=false;
                try {probe.startCalibration(group,request,[]{});} catch(const fan::calibration::SafetyRefusal&) {refused=true;}
                if(!refused) throw std::runtime_error("Override leaked to next calibration attempt");
            }
            std::cout<<nlohmann::json{{"started",started},{"progress",progress},{"aborted",aborted},{"after",after}}.dump()<<'\n';
            return 0;
        }
        if(std::string(argv[2])=="abort") {
            std::cout<<probe.abortCalibration().dump()<<'\n';return 0;
        }
        if(std::string(argv[2])=="ui") {
            auto before=probe.snapshot();
            auto after=probe.updateGroup(0,{{"configGeneration",1},{"calibrationGeneration",7},{"changes",{{"faultDelaySeconds",5}}}});
            std::cout<<nlohmann::json{{"before",before},{"after",after}}.dump()<<'\n';
            return 0;
        }
        if(std::string(argv[2])=="claim") {
            probe.setIdentity("1234567890abcdef1234567890abcdef"); hello=probe.hello();
        }
        std::cout<<hello.dump()<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
