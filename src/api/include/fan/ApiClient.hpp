#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>

namespace fan {
class ApiError : public std::runtime_error {
public:
    ApiError(int status, nlohmann::json body):
        std::runtime_error("Daemon: "+body.value("error",std::to_string(status))),statusCode(status),details(std::move(body)) {}
    int statusCode;
    nlohmann::json details;
};
class ApiClient {
public:
    explicit ApiClient(int port = 8787) : port_(port) {}
    nlohmann::json status() const;
    nlohmann::json alerts() const;
    nlohmann::json thermalLimits(const std::string& pci) const;
    nlohmann::json discover() const;
    nlohmann::json config() const;
    nlohmann::json save(const nlohmann::json& config) const;
    nlohmann::json claim(const std::string& path) const;
    nlohmann::json snapshot(const std::string& id) const;
    nlohmann::json liveStatus(const std::string& id) const;
    nlohmann::json updateSupply(const std::string& id,const nlohmann::json& body) const;
    nlohmann::json updateAdapterNames(const std::string& id,const nlohmann::json& body) const;
    nlohmann::json acknowledgeLatched(const std::string& id,const nlohmann::json& body) const;
    nlohmann::json calibrationProgress(const std::string& id) const;
    nlohmann::json startCalibration(const std::string& id, int group, const nlohmann::json& request) const;
    nlohmann::json abortCalibration(const std::string& id) const;
    nlohmann::json updateGroup(const std::string& id, int group, const nlohmann::json& request) const;
    nlohmann::json setGroupMode(const std::string& id,int group,const nlohmann::json& request) const;
private:
    nlohmann::json request(const std::string& method, const std::string& path,
                           const nlohmann::json& body = nullptr) const;
    int port_;
};
}
