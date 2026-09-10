#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <atomic>
#include "fan/Protocol.hpp"
#include "WakeSignal.hpp"

namespace fan {
class SerialProbe {
public:
    explicit SerialProbe(const std::string& path);
    ~SerialProbe();
    SerialProbe(const SerialProbe&) = delete;
    SerialProbe& operator=(const SerialProbe&) = delete;
    nlohmann::json hello();
    void setIdentity(const std::string& id);
    nlohmann::json snapshot();
    nlohmann::json status();
    nlohmann::json liveStatus();
    nlohmann::json updateSupply(const nlohmann::json& request);
    nlohmann::json updateAdapterNames(const nlohmann::json& request);
    nlohmann::json clearLatched(const nlohmann::json& request);
    nlohmann::json startCalibration(int group, const nlohmann::json& request,
        const std::function<void()>& requireLiveForwarding);
    nlohmann::json abortCalibration();
    nlohmann::json updateGroup(int group, const nlohmann::json& request);
    nlohmann::json setGroupMode(int group,const nlohmann::json& request);
    nlohmann::json temperatures(const std::function<protocol::TemperatureSnapshot()>& latest,
        std::chrono::steady_clock::time_point& transmitted);
    // Nonblocking idle receive; never transmits a heartbeat or competes with a queued temperature.
    void receiveEvents();
    bool hasPendingEvents();
    void waitForActivity(const WakeSignal& wake,std::stop_token stop,
        std::chrono::steady_clock::time_point deadline);
    nlohmann::json takeEvents();
private:
    std::atomic<unsigned> capabilities_{0};
    std::vector<std::uint8_t> receiveBuffer_;
    std::chrono::steady_clock::time_point lastByte_{};
    std::mutex eventsMutex_;
    nlohmann::json events_=nlohmann::json::array();
    unsigned droppedEvents_=0;
    struct Frame { std::uint8_t type; unsigned short sequence; std::vector<std::uint8_t> payload; };
    std::vector<Frame> readFrames();
    void retain(const Frame& frame);
    nlohmann::json decodeStatus(const std::vector<std::uint8_t>& bytes) const;
    std::mutex gate_;
    std::condition_variable available_;
    bool busy_=false;
    unsigned priorityWaiting_=0;
    WakeSignal activity_;
    int fd_ = -1;
    unsigned short sequence_ = 0;
    std::vector<std::uint8_t> exchange(protocol::MessageType type,
        protocol::MessageType reply, const std::vector<std::uint8_t>& payload = {},
        const std::function<std::vector<std::uint8_t>()>& latest = {},
        std::chrono::steady_clock::time_point* transmitted = nullptr, bool onlyIdle = false);
};
}
