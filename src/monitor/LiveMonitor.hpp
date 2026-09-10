#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <vector>

namespace fan {
// UI-owned, demand-driven polling. No daemon subscriptions or hardware access.
class LiveMonitor {
public:
    using Json=nlohmann::json;
    using Clock=std::chrono::steady_clock;
    using Read=std::function<Json(const std::string&)>;
    struct Sample {
        Json status=nullptr;
        std::string error;
        Clock::time_point received{},next{};
        bool busy=false;
        bool fresh(Clock::time_point now)const {return !status.is_null() && error.empty() && now-received<std::chrono::seconds(5);}
    };
    explicit LiveMonitor(Read read):read_(std::move(read)) {}
    void tick(const std::vector<std::string>& ids,bool launch);
    const Sample* sample(const std::string& id)const;
private:
    struct Result {Json status=nullptr;std::string error;Clock::time_point received;};
    struct Job {std::string id;std::future<Result> future;};
    Read read_;
    std::map<std::string,Sample> samples_;
    std::vector<Job> jobs_;
    std::size_t cursor_=0;
};
}
