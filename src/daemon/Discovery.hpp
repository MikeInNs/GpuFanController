#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <map>
#include <memory>
#include <stop_token>
#include "SerialProbe.hpp"

namespace fan {
class Discovery {
public:
    nlohmann::json scan(std::stop_token stop={});
    nlohmann::json claim(const std::string& path);
    SerialProbe& connected(const std::string& path);
    void disconnect(const std::string& path) { probes_.erase(path); }
    std::shared_ptr<SerialProbe> share(const std::string& path) const {
        auto it=probes_.find(path);return it==probes_.end()?nullptr:it->second;
    }
    void disconnect(const std::shared_ptr<SerialProbe>& probe) {
        std::erase_if(probes_,[&](const auto& entry){return entry.second==probe;});
    }
private:
    std::map<std::string,std::shared_ptr<SerialProbe>> probes_;
};
}
