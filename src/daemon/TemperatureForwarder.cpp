#include "TemperatureForwarder.hpp"
#include "TemperatureRetry.hpp"
#include <iostream>
#include <syncstream>

namespace fan {
void TemperatureForwarder::pause(std::stop_token stop,std::chrono::milliseconds duration) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    wake.wait_for(lock,stop,duration,[]{return false;});
}
void TemperatureForwarder::wakeWorkers() {
    std::lock_guard lock(mutex_);
    for(const auto& [id,w]:workers_) {(void)id;w->wake.notify();}
}
TemperatureForwarder::TemperatureForwarder(bool enabled,Connect connect,Disconnect disconnect,Scan scan,Query nvidia,Query amd)
    :enabled_(enabled),connect_(std::move(connect)),disconnect_(std::move(disconnect)),scan_(std::move(scan)),queries_{std::move(nvidia),std::move(amd)} {
    if(!enabled_) return;
    if(!queries_[0]) {
        nvidiaReader_=std::make_shared<NvmlReader>();
        queries_[0]=[reader=nvidiaReader_](const auto& addresses){return reader->sample(addresses);};
    }
    for(std::size_t index=0;index<samplers_.size();++index) samplers_[index]=std::jthread([this,index](std::stop_token stop) {
        TemperatureRetry retry;
        while(!stop.stop_requested()) {
            samplerWake_[index].clear();
            std::set<std::string> addresses;
            {std::lock_guard lock(mutex_);for(const auto& [id,w]:workers_) {
                (void)id;std::lock_guard guard(w->mutex);
                for(const auto& group:w->mapping.at("groups"))
                    if(group.at("enabled").get<bool>()) addresses.insert(group.at("gpuPciAddress").get<std::string>());
            }}
            if(!addresses.empty()) {
                const auto cycleStarted=SteadyClock::now();retry.begin();
                for(unsigned attempt=1;attempt<=TemperatureRetry::maxAttempts && !stop.stop_requested();++attempt) {
                    const auto started=SteadyClock::now();GpuReadings next;
                    {std::lock_guard lock(sampleMutex_);
                        auto& timing=queryTimings_[index];timing.started=started;timing.inFlight=true;timing.attempt=attempt;
                    }
                    try {next=queries_[index](addresses);}
                    catch(const std::exception& e) {next={{},started,e.what()};}
                    const auto completed=SteadyClock::now();
                    const bool again=retry.accept(next);
                    const GpuQueryTiming timing{started,std::chrono::duration_cast<std::chrono::milliseconds>(completed-started),false,attempt,again};
                    {std::lock_guard lock(sampleMutex_);readings_[index]=retry.readings();queryTimings_[index]=timing;}
                    wakeWorkers();
                    // Intermediate failures (and recovery inside the budget) are quiet.
                    diagnostics_.queryCompleted(index,next,timing,completed);
                    if(!again) break;
                    pause(stop,TemperatureRetry::delay);
                }
                samplerWake_[index].wait(stop,cycleStarted+std::chrono::seconds(1));
            } else {
                if(index==0 && nvidiaReader_) nvidiaReader_->stop();
                retry.clear();
                {std::lock_guard lock(sampleMutex_);readings_[index]={};queryTimings_[index]={};}
                wakeWorkers();
                samplerWake_[index].wait(stop,SteadyClock::time_point::max());
            }
        }
        // Release NVML before its spawning thread exits (PDEATHSIG is thread-bound).
        if(index==0 && nvidiaReader_) nvidiaReader_->stop();
    });
    scanner_=std::jthread([this](std::stop_token stop) {
        auto nextScan=SteadyClock::time_point{};
        while(!stop.stop_requested()) {
            scannerWake_.clear();
            bool needed=false;
            {std::lock_guard lock(mutex_);for(const auto& [id,w]:workers_) { (void)id;std::lock_guard guard(w->mutex);needed|=!w->state.value("connected",false);}}
            if(needed) {
                if(SteadyClock::now()<nextScan) {scannerWake_.wait(stop,nextScan);continue;}
                try {scan_(stop);} catch(const std::exception& e) {std::osyncstream(std::cerr)<<"Controller reconnect scan: "<<e.what()<<'\n';}
                nextScan=SteadyClock::now()+std::chrono::seconds(5);
                wakeWorkers();
                scannerWake_.wait(stop,nextScan);
            } else scannerWake_.wait(stop,SteadyClock::time_point::max());
        }
    });
}
TemperatureForwarder::~TemperatureForwarder() {
    for(auto& sampler:samplers_) sampler.request_stop();
    scanner_.request_stop();
    std::vector<std::shared_ptr<Worker>> workers;
    {std::lock_guard lock(mutex_);for(auto& [id,w]:workers_) {(void)id;w->thread.request_stop();workers.push_back(w);}workers_.clear();}
    for(auto& w:workers) if(w->thread.joinable()) w->thread.join();
    if(scanner_.joinable()) scanner_.join();
    for(auto& sampler:samplers_) if(sampler.joinable()) sampler.join();
}
void TemperatureForwarder::configure(const Json& config) {
    alerts_.configure(config,enabled_);
    if(!enabled_) return;
    std::vector<std::shared_ptr<Worker>> removed;
    {
        std::lock_guard lock(mutex_);
        std::map<std::string,bool> wanted;
        for(const auto& c:config.at("controllers")) {
            const auto id=c.at("controllerId").get<std::string>();wanted[id]=true;
            if(auto found=workers_.find(id);found!=workers_.end()) {
                std::lock_guard guard(found->second->mutex);found->second->mapping=c;
                found->second->wake.notify();
            } else {
                auto w=std::make_shared<Worker>();w->mapping=c;
                workers_[id]=w;w->thread=std::jthread([this,w](std::stop_token stop){run(*w,stop);});
            }
        }
        for(auto it=workers_.begin();it!=workers_.end();) {
            if(!wanted.contains(it->first)) {it->second->thread.request_stop();removed.push_back(it->second);it=workers_.erase(it);}
            else ++it;
        }
    }
    for(auto& wake:samplerWake_) wake.notify();
    scannerWake_.notify();
    for(auto& w:removed) {
        if(w->thread.joinable()) w->thread.join();
        diagnostics_.forget(w->mapping.at("controllerId"));
    }
}
void TemperatureForwarder::run(Worker& worker,std::stop_token stop) {
    std::shared_ptr<SerialProbe> probe;
    TemperatureSchedule schedule;
    std::optional<std::uint32_t> previousUptime;
    bool reconcile=false;
    bool identityVerified=false;
    auto retryAt=SteadyClock::time_point{};
    const auto started=SteadyClock::now();
    while(!stop.stop_requested()) {
        worker.wake.clear();
        Json mapping;{std::lock_guard lock(worker.mutex);mapping=worker.mapping;}
        try {
            if(!probe) {
                if(SteadyClock::now()<retryAt) {worker.wake.wait(stop,retryAt);continue;}
                probe=connect_(mapping.at("controllerId"));
                if(!probe) {
                    if(SteadyClock::now()-started>std::chrono::seconds(10)) alerts_.connection(mapping.at("controllerId"),false);
                    // A scan/config update wakes us sooner. The bounded retry
                    // also covers a connection lookup skipped by a busy UI.
                    worker.wake.wait(stop,SteadyClock::now()+std::chrono::seconds(1));continue;
                }
                if(probe->hello().at("controllerId")!=mapping.at("controllerId")) throw std::runtime_error("Controller identity mismatch");
                identityVerified=true;
                // Full state is queried once on connection, not every temperature send.
                const auto initial=probe->snapshot();
                {std::lock_guard lock(worker.mutex);worker.state["startupStatus"]=initial.at("status");}
                schedule.reset();
                previousUptime.reset();
                alerts_.connection(mapping.at("controllerId"),true);
                scannerWake_.notify();
            }
            if(reconcile) {
                const auto fresh=probe->snapshot();
                {std::lock_guard lock(worker.mutex);worker.state["startupStatus"]=fresh.at("status");}
                reconcile=false;
            }
            std::array<GpuReadings,2> readings;{std::lock_guard lock(sampleMutex_);readings=readings_;}
            auto snapshot=mappedTemperatures(mapping.at("groups"),readings,SteadyClock::now());
            if(schedule.due(snapshot,SteadyClock::now())) {
                SteadyClock::time_point transmitted,selectedAt;
                std::int64_t selectedTimestamp=0;
                std::array<GpuQueryTiming,2> timings;
                Json heartbeat;
                auto logPacket=[&](bool replyReceived) {
                    if(transmitted!=SteadyClock::time_point{})
                        diagnostics_.packet(mapping.at("controllerId"),mapping.at("groups"),snapshot,readings,timings,selectedAt,selectedTimestamp,replyReceived);
                };
                try {heartbeat=probe->temperatures([&] {
                    // Recheck freshness after waiting for the serial slot.
                    std::lock_guard lock(sampleMutex_);
                    selectedAt=SteadyClock::now();selectedTimestamp=TemperatureDiagnostics::timestampMs();
                    readings=readings_;timings=queryTimings_;
                    snapshot=mappedTemperatures(mapping.at("groups"),readings,selectedAt);
                    return snapshot;
                },transmitted);} catch(...) {logPacket(false);throw;}
                // Keep logging/JSON work outside the serial slot and sample lock.
                logPacket(true);
                schedule.sent(snapshot,transmitted);
                const auto uptime=heartbeat.at("uptimeMs").get<std::uint32_t>();
                reconcile=previousUptime && uptime<*previousUptime;
                previousUptime=uptime;
                std::lock_guard lock(worker.mutex);
                worker.lastAck=SteadyClock::now();
                worker.state["connected"]=true;worker.state["error"]="";
                worker.state["validMask"]=snapshot.valid_mask;
                worker.state["temperaturesDeciC"]=Json::array({snapshot.valid_mask&1?Json(snapshot.group1_deci_celsius):Json(nullptr),snapshot.valid_mask&2?Json(snapshot.group2_deci_celsius):Json(nullptr)});
                worker.state["heartbeat"]=heartbeat;
            }
            probe->receiveEvents();
            if(probe->hasPendingEvents()) alerts_.ingest(mapping.at("controllerId"),probe->takeEvents());
            if(reconcile) continue;
            const auto now=SteadyClock::now();
            auto deadline=schedule.nextDue(mappedTemperatures(mapping.at("groups"),readings,now),now);
            // Expiry is a timer even when a provider is blocked and cannot
            // notify us. Ignore already-expired samples to avoid an idle spin.
            for(const auto& group:mapping.at("groups")) {
                if(!group.at("enabled").get<bool>()) continue;
                const auto pci=group.at("gpuPciAddress").get<std::string>();
                for(const auto& reading:readings) if(reading.temperatures.contains(pci)) {
                    const auto expiry=reading.timestampFor(pci)+std::chrono::seconds(3);
                    if(expiry>now) deadline=std::min(deadline,expiry);
                }
            }
            probe->waitForActivity(worker.wake,stop,deadline);
        } catch(const std::exception& e) {
            if(probe) {
                const auto batch=probe->takeEvents();
                if(identityVerified) alerts_.ingest(mapping.at("controllerId"),batch);
                disconnect_(probe);
            }
            alerts_.connection(mapping.at("controllerId"),false);
            identityVerified=false;
            probe.reset();schedule.reset();reconcile=false;retryAt=SteadyClock::now()+std::chrono::seconds(5);
            std::lock_guard lock(worker.mutex);
            if(worker.state.value("error",std::string{})!=e.what()) std::osyncstream(std::cerr)<<"Controller "<<mapping.at("controllerId")<<": "<<e.what()<<'\n';
            worker.state["connected"]=false;worker.state["error"]=e.what();
            scannerWake_.notify();
        }
    }
}
TemperatureForwarder::Json TemperatureForwarder::status() const {
    Json result{{"enabled",enabled_},{"controllers",Json::array()},{"ready",false}};
    bool ready=true;
    std::lock_guard lock(mutex_);
    for(const auto& [id,w]:workers_) {
        std::lock_guard guard(w->mutex);auto state=w->state;state["controllerId"]=id;
        const auto age=w->lastAck==SteadyClock::time_point{}? -1:std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now()-w->lastAck).count();
        state["lastReplyAgeMs"]=age<0?Json(nullptr):Json(age);
        state["responsive"]=state.value("connected",false) && age>=0 && age<10000;
        int required=0;for(int g=0;g<2;++g) if(w->mapping["groups"][g]["enabled"].get<bool>()) required|=1<<g;
        state["requiredValidMask"]=required;
        ready=ready && state["responsive"].get<bool>() && state.value("validMask",-1)==required;
        result["controllers"].push_back(state);
    }
    result["ready"]=enabled_ && !workers_.empty() && ready;
    result["gpuDiagnostics"]=diagnostics_.history();
    {std::lock_guard sampleLock(sampleMutex_);
        std::string errors;
        for(std::size_t i=0;i<readings_.size();++i) if(!readings_[i].error.empty()) {
            if(!errors.empty()) errors+="; ";
            errors+=(i==0?"NVIDIA: ":"AMD: ")+readings_[i].error;
        }
        result["gpuError"]=errors;
    }
    return result;
}
}
