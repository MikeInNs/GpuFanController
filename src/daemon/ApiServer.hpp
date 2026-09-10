#pragma once
#include <httplib.h>
#include "ConfigStore.hpp"
#include "Discovery.hpp"
#include "TemperatureForwarder.hpp"
#include <mutex>

namespace fan {
class ApiServer {
public:
    explicit ApiServer(const std::filesystem::path& configPath,bool forwarding=true);
    int bind(int port);
    bool listen();
    void stop();
private:
    httplib::Server server_;
    ConfigStore config_;
    Discovery discovery_;
    std::mutex mutex_;
    std::mutex thermalMutex_; // Bound explicit setup queries without locking serial workers.
    nlohmann::json inventory_{{"controllers",nlohmann::json::array()},
        {"gpus",nlohmann::json::array()},{"errors",nlohmann::json::array()}};
    std::unique_ptr<TemperatureForwarder> forwarding_;
};
}
