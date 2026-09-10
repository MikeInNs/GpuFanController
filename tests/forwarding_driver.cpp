#include "TemperatureForwarder.hpp"
#include "CalibrationControl.hpp"
#include <atomic>
#include <iostream>
using namespace std::chrono_literals;
int main(int argc,char** argv) try {
    if(argc!=3 && argc!=4) return 2;
    const bool calibrate=argc==4 && std::string(argv[3])=="calibration";
    const bool control=argc==4 && std::string(argv[3])=="groups";
    const bool live=argc==4 && std::string(argv[3])=="live";
    const bool mixed=argc==4 && std::string(argv[3])=="mixed";
    const bool retries=argc==4 && std::string(argv[3])=="retries";
    using fan::SteadyClock;
    using Json=nlohmann::json;
    const auto started=SteadyClock::now();
    std::mutex mutex;
    std::map<std::string,std::weak_ptr<fan::SerialProbe>> connected;
    std::atomic<unsigned> scans=0;
    Json trace=Json::array();
    Json queries=Json::array();unsigned transientFailures=0;
    auto sampleData=[&] {
        const auto sampledAt=SteadyClock::now();
        const auto csv=[&]() -> std::string {
            const auto seconds=std::chrono::duration<double>(sampledAt-started).count();
            if(seconds>=10 && seconds<12) throw std::runtime_error("Simulated driver failure");
            if(seconds>=12) return "00000000:01:00.0, 52\n00000000:02:00.0, 61\n";
            if(seconds>=9) return "00000000:01:00.0, N/A\n00000000:02:00.0, 60\n";
            return seconds>=8?"00000000:01:00.0, 49.1\n00000000:02:00.0, 60\n":"00000000:01:00.0, 49\n00000000:02:00.0, 60\n";
        }();
        return fan::NvidiaTemperatures::parse(csv,sampledAt);
    };
    {
        fan::TemperatureForwarder runtime(true,[&](const std::string& id) {
            auto probe=std::make_shared<fan::SerialProbe>(id==std::string(32,'a')?argv[1]:argv[2]);
            std::lock_guard lock(mutex);connected[id]=probe;return probe;
        },[](const auto&){},[&](std::stop_token){++scans;},[&](const auto&) {
            const auto age=std::chrono::duration<double>(SteadyClock::now()-started).count();
            try {
                if(retries && age>=7 && age<8 && transientFailures<2) {
                    ++transientFailures;throw std::runtime_error("Transient retry fixture");
                }
                auto data=sampleData();
                if(mixed) data.temperatures.erase("0000:02:00.0");
                queries.push_back({{"at",age},{"error",data.error}});return data;
            } catch(const std::exception& e) {queries.push_back({{"at",age},{"error",e.what()}});throw;}
        },[&](const auto&) {
            if(!mixed) return fan::GpuReadings{};
            auto data=sampleData();data.temperatures.erase("0000:01:00.0");return data;
        });
        Json both=Json::array({{{"enabled",true},{"gpuPciAddress","0000:01:00.0"}},{{"enabled",true},{"gpuPciAddress","0000:02:00.0"}}});
        Json single=Json::array({{{"enabled",true},{"gpuPciAddress","0000:02:00.0"}},{{"enabled",false},{"gpuPciAddress",nullptr}}});
        Json config{{"controllers",Json::array({{{"controllerId",std::string(32,'a')},{"groups",both}},{{"controllerId",std::string(32,'b')},{"groups",single}}})}};
        runtime.configure(config);
        bool uiRead=retries,remapped=false,removed=false;
        std::jthread ui;
        while(SteadyClock::now()-started<20s) {
            const auto age=std::chrono::duration<double>(SteadyClock::now()-started).count();
            if(!uiRead && age>=6.5) {
                uiRead=true;std::shared_ptr<fan::SerialProbe> probe;
                {std::lock_guard lock(mutex);probe=connected[std::string(32,'a')].lock();}
                ui=std::jthread([probe,calibrate,control,live,&runtime]{
                    if(!probe) return;
                    if(live) {
                        for(int i=0;i<5;++i) {
                            try {probe->liveStatus();} catch(const fan::ConfigConflict&) {}
                            std::this_thread::sleep_for(1s);
                        }
                        return;
                    }
                    if(control) {
                        for(const auto* mode:{"full","off","auto"}) {
                            probe->setGroupMode(0,{{"confirmed",true},{"configGeneration",1},{"mode",mode},{"timeoutSeconds",std::string(mode)=="auto"?0:300}});
                            std::this_thread::sleep_for(1s);
                        }
                        return;
                    }
                    if(!calibrate) {probe->snapshot();return;}
                    probe->startCalibration(0,{{"confirmed",true},{"configGeneration",1},{"calibrationGeneration",7}},
                        [&]{fan::calibration::requireForwarding(runtime.status(),std::string(32,'a'),0);});
                    for(int i=0;i<5;++i) {probe->status();std::this_thread::sleep_for(1s);}
                    probe->abortCalibration();probe->snapshot();
                });
            }
            if(!remapped && age>=15.5) {remapped=true;config["controllers"][0]["groups"]=single;runtime.configure(config);}
            if(!removed && age>=18) {removed=true;config["controllers"].erase(1);runtime.configure(config);}
            auto state=runtime.status();
            state["alerts"]=runtime.alerts();
            for(auto& c:state["controllers"]) c.erase("startupStatus");
            trace.push_back({{"at",age},{"state",state}});
            std::this_thread::sleep_for(250ms);
        }
    }
    std::cout<<Json{{"trace",trace},{"scans",scans.load()},{"queries",queries}}.dump()<<'\n';
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
