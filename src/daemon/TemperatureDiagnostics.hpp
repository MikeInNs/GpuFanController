#pragma once
#include "GpuTemperatures.hpp"
#include <array>
#include <mutex>

namespace fan {
struct GpuQueryTiming {
    SteadyClock::time_point started{};
    std::optional<std::chrono::milliseconds> duration;
    bool inFlight=false;
    unsigned attempt=1;
    bool retryPending=false;
};
// Host diagnostics only: never generates Nano alarms or changes validity policy.
class TemperatureDiagnostics {
public:
    using Json=nlohmann::json;
    void queryCompleted(std::size_t provider,const GpuReadings& reading,
        const GpuQueryTiming& timing,SteadyClock::time_point now);
    void packet(const std::string& controller,const Json& groups,
        const protocol::TemperatureSnapshot& snapshot,const std::array<GpuReadings,2>& readings,
        const std::array<GpuQueryTiming,2>& timings,SteadyClock::time_point selectedAt,
        std::int64_t timestampMs,bool replyReceived);
    void forget(const std::string& controller);
    Json history() const;
    static std::int64_t timestampMs();
private:
    void record(Json event); // mutex_ held
    mutable std::mutex mutex_;
    Json history_=Json::array();
    std::array<std::string,2> errors_;
    std::map<std::string,std::array<Json,2>> previous_;
};
}
