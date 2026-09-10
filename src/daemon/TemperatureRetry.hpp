#pragma once
#include "GpuTemperatures.hpp"

namespace fan {
// Per-provider retry/cache policy. Scheduling, serial I/O and alarms live elsewhere.
class TemperatureRetry {
public:
    static constexpr unsigned maxAttempts=3;
    static constexpr auto delay=std::chrono::milliseconds(250);
    void begin() {attempt_=0;}
    void clear() {attempt_=0;published_={};}
    bool accept(GpuReadings next) {
        ++attempt_;
        const bool retry=!next.error.empty() && attempt_<maxAttempts;
        // Publish good channels immediately, retaining only unavailable channels
        // during retry. Never move a retained measurement's original timestamp.
        if(retry) for(const auto& [pci,temperature]:published_.temperatures) {
            if(next.temperatures.contains(pci)) continue;
            next.temperatures[pci]=temperature;
            next.sampleTimes[pci]=published_.timestampFor(pci);
        }
        published_=std::move(next);
        return retry;
    }
    const GpuReadings& readings() const {return published_;}
private:
    unsigned attempt_=0;
    GpuReadings published_;
};
}
