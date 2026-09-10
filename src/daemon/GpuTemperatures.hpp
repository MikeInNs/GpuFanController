#pragma once
#include "fan/Protocol.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <set>
#include <span>

namespace fan {
using SteadyClock=std::chrono::steady_clock;
struct GpuReadings {
    std::map<std::string,std::int16_t> temperatures;
    SteadyClock::time_point sampledAt{};
    std::string error;
    // Retried partial batches can contain readings from different query times.
    std::map<std::string,SteadyClock::time_point> sampleTimes{};
    SteadyClock::time_point timestampFor(const std::string& pci) const {
        const auto found=sampleTimes.find(pci);
        return found==sampleTimes.end()?sampledAt:found->second;
    }
};
class NvidiaTemperatures {
public:
    // CSV decoder retained for deterministic sampler fixtures, not live polling.
    static GpuReadings parse(const std::string& csv, SteadyClock::time_point sampledAt);
};
protocol::TemperatureSnapshot mappedTemperatures(const nlohmann::json& groups,
    const GpuReadings& readings, SteadyClock::time_point now);
protocol::TemperatureSnapshot mappedTemperatures(const nlohmann::json& groups,
    std::span<const GpuReadings> sources, SteadyClock::time_point now);
class TemperatureSchedule {
public:
    bool due(const protocol::TemperatureSnapshot& snapshot, SteadyClock::time_point now) const;
    SteadyClock::time_point nextDue(const protocol::TemperatureSnapshot& snapshot, SteadyClock::time_point now) const;
    void sent(const protocol::TemperatureSnapshot& snapshot, SteadyClock::time_point now);
    void reset() {previous_.reset();}
private:
    std::optional<std::vector<std::uint8_t>> previous_;
    SteadyClock::time_point sentAt_{};
};
}
