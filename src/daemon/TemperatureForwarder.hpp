#pragma once
#include "GpuTemperatures.hpp"
#include "AmdGpu.hpp"
#include "SerialProbe.hpp"
#include "AlertCenter.hpp"
#include "TemperatureDiagnostics.hpp"
#include "NvmlReader.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <array>

namespace fan {
class TemperatureForwarder {
public:
    using Json=nlohmann::json;
    using Connect=std::function<std::shared_ptr<SerialProbe>(const std::string&)>;
    using Disconnect=std::function<void(const std::shared_ptr<SerialProbe>&)>;
    using Scan=std::function<void(std::stop_token)>;
    using Query=std::function<GpuReadings(const std::set<std::string>&)>;
    TemperatureForwarder(bool enabled,Connect connect,Disconnect disconnect,Scan scan,
        Query nvidia={},
        Query amd=[](const auto& addresses){return AmdGpu{}.sample(addresses);});
    ~TemperatureForwarder();
    void configure(const Json& config);
    Json status() const;
    Json alerts() const {return alerts_.status();}
private:
    struct Worker {
        mutable std::mutex mutex;
        Json mapping,state{{"connected",false},{"error","Waiting for controller"}};
        SteadyClock::time_point lastAck{};
        WakeSignal wake;
        std::jthread thread;
    };
    void run(Worker& worker,std::stop_token stop);
    void wakeWorkers();
    bool enabled_;
    Connect connect_;
    Disconnect disconnect_;
    Scan scan_;
    std::array<Query,2> queries_;
    std::shared_ptr<NvmlReader> nvidiaReader_;
    AlertCenter alerts_;
    TemperatureDiagnostics diagnostics_;
    mutable std::mutex mutex_,sampleMutex_;
    std::map<std::string,std::shared_ptr<Worker>> workers_;
    std::array<GpuReadings,2> readings_;
    std::array<GpuQueryTiming,2> queryTimings_;
    std::array<WakeSignal,2> samplerWake_;
    WakeSignal scannerWake_;
    std::array<std::jthread,2> samplers_;
    std::jthread scanner_;
    static void pause(std::stop_token stop,std::chrono::milliseconds duration);
};
}
