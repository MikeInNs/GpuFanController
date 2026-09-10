#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace fan {
// Notification only: no serial access or fan-control decisions in this worker.
class AudibleAlarm {
public:
    using Sound=std::function<void()>;
    explicit AudibleAlarm(Sound sound=pcSpeaker,
        std::chrono::milliseconds interval=std::chrono::seconds(30));
    ~AudibleAlarm();
    void enable(bool enabled);
    void update(bool criticalActive,bool criticalRaised);
    nlohmann::json status() const;
    static void pcSpeaker();
private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool enabled_=false,active_=false,pending_=false,stopping_=false;
    Sound sound_;
    std::chrono::milliseconds interval_;
    nlohmann::json state_{{"state","disabled"},{"error",""},{"attempts",0},{"lastSuccessAtMs",nullptr}};
    std::thread worker_;
    void run();
};
}
