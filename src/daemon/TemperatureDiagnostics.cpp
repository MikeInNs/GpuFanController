#include "TemperatureDiagnostics.hpp"
#include "TemperatureRetry.hpp"
#include <iostream>
#include <syncstream>

namespace fan {
namespace {
using Json=nlohmann::json;
Json age(SteadyClock::time_point time,SteadyClock::time_point now) {
    if(time==SteadyClock::time_point{} || now<time) return nullptr;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now-time).count();
}
Json source(std::size_t index,const GpuReadings& reading,const GpuQueryTiming& timing,SteadyClock::time_point now) {
    return {{"provider",index==0?"NVIDIA":"AMD"},{"sampleAgeMs",age(reading.sampledAt,now)},
        {"queryDurationMs",timing.duration?Json(timing.duration->count()):Json(nullptr)},
        {"queryInFlight",timing.inFlight},
        {"attempt",timing.attempt},{"maxAttempts",TemperatureRetry::maxAttempts},{"retryPending",timing.retryPending},
        {"queryInFlightAgeMs",timing.inFlight?age(timing.started,now):Json(nullptr)},
        {"error",reading.error.empty()?Json(nullptr):Json(reading.error.substr(0,1024))}};
}
}
std::int64_t TemperatureDiagnostics::timestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void TemperatureDiagnostics::record(Json event) {
    if(!event.contains("timestampMs")) event["timestampMs"]=timestampMs();
    // Escape driver output (including newlines/control characters) as one JSON log line.
    std::osyncstream(std::cerr)<<"GPU temperature diagnostic "<<event.dump(-1,' ',false,Json::error_handler_t::replace)<<'\n';
    if(history_.size()==256) history_.erase(history_.begin());
    history_.push_back(std::move(event));
}
void TemperatureDiagnostics::queryCompleted(std::size_t provider,const GpuReadings& reading,
    const GpuQueryTiming& timing,SteadyClock::time_point now) {
    if(timing.retryPending) return;
    std::lock_guard lock(mutex_);
    const auto error=reading.error.substr(0,1024);
    if(errors_.at(provider)==error) return;
    auto event=source(provider,reading,timing,now);
    event["kind"]="query";event["transition"]=error.empty()?"recovered":"error";
    event["previousError"]=errors_[provider].empty()?Json(nullptr):Json(errors_[provider]);
    errors_[provider]=error;record(std::move(event));
}
void TemperatureDiagnostics::packet(const std::string& controller,const Json& groups,
    const protocol::TemperatureSnapshot& snapshot,const std::array<GpuReadings,2>& readings,
    const std::array<GpuQueryTiming,2>& timings,SteadyClock::time_point selectedAt,
    std::int64_t timestamp,bool replyReceived) {
    std::lock_guard lock(mutex_);
    auto& previous=previous_[controller];
    for(std::size_t g=0;g<2;++g) {
        const auto& mapping=groups.at(g);
        const bool enabled=mapping.at("enabled").get<bool>();
        const bool valid=(snapshot.valid_mask&(1u<<g))!=0;
        Json state{{"enabled",enabled},{"pciAddress",mapping.at("gpuPciAddress")},{"valid",valid}};
        if(state==previous[g]) continue;
        const auto old=previous[g];previous[g]=state;
        if(!enabled && old.is_null()) continue; // An unused group is not a fault.
        const auto pci=mapping.at("gpuPciAddress").is_string()?mapping.at("gpuPciAddress").get<std::string>():std::string{};
        Json sources=Json::array();unsigned fresh=0;bool expired=false;
        for(std::size_t i=0;i<readings.size();++i) {
            auto detail=source(i,readings[i],timings[i],selectedAt);
            const bool present=readings[i].temperatures.contains(pci);
            detail["sampleAgeMs"]=present?age(readings[i].timestampFor(pci),selectedAt):Json(nullptr);
            const auto sampleAge=detail.at("sampleAgeMs");
            const bool usable=present && !sampleAge.is_null() && sampleAge.get<std::int64_t>()<3000;
            fresh+=usable;expired|=present && !usable;
            detail["containsGpu"]=present;detail["freshForGpu"]=usable;sources.push_back(std::move(detail));
        }
        const char* reason=!enabled?"mapping_disabled":valid?"valid":fresh>1?"duplicate_readings":expired?"sample_expired":"reading_unavailable";
        const char* transition=!enabled?"disabled":old.is_null()?(valid?"initial_valid":"initial_invalid"):
            old.at("pciAddress")!=state.at("pciAddress") || old.at("enabled")!=enabled?"mapping_changed":valid?"recovered":"invalid";
        record({{"kind","forwarded_validity"},{"timestampMs",timestamp},{"controllerId",controller},
            {"group",g+1},{"pciAddress",state.at("pciAddress")},{"transition",transition},{"valid",valid},
            {"reason",reason},{"replyReceived",replyReceived},{"sources",sources}});
    }
}
void TemperatureDiagnostics::forget(const std::string& controller) {
    std::lock_guard lock(mutex_);previous_.erase(controller);
}
TemperatureDiagnostics::Json TemperatureDiagnostics::history() const {
    std::lock_guard lock(mutex_);return history_;
}
}
