#pragma once
#include "AudibleAlarm.hpp"
#include <array>
#include <map>

namespace fan {
class AlertCenter {
public:
    using Json=nlohmann::json;
    void configure(const Json& config,bool monitoring);
    void ingest(const std::string& id,const Json& batch);
    void connection(const std::string& id,bool connected);
    Json status() const;
private:
    struct Controller {
        std::array<unsigned,3> masks{},latched{};
        bool known=false,connected=false,transportFault=false,historyLost=false;
        Json lastObserved=nullptr;
    };
    mutable std::mutex mutex_;
    std::map<std::string,Controller> controllers_;
    Json history_=Json::array();
    std::uint64_t sequence_=0;
    AudibleAlarm audible_;
    void record(const std::string& id,int scope,unsigned bit,bool raised,const char* source,std::int64_t at);
    bool critical() const;
    static Json description(int scope,unsigned bit);
};
}
