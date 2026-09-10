#include "fan/ApiClient.hpp"
#include <httplib.h>
#include <stdexcept>

namespace fan {
nlohmann::json ApiClient::request(const std::string& method, const std::string& path, const nlohmann::json& body) const {
    httplib::Client client("127.0.0.1", port_);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(45, 0); // A bounded scan may wait for eight Nano bootloaders.
    if(path=="/api/v1/alerts") client.set_read_timeout(2,0); // Cached feed never scans hardware.
    if(path.starts_with("/api/v1/gpus/")) client.set_read_timeout(5,0);
    if(path.starts_with("/api/v1/controllers/") && path.ends_with("/status")) client.set_read_timeout(3,0);
    auto response = method == "POST" ? client.Post(path,body.dump(),"application/json") :
        method == "PUT" ? client.Put(path,body.dump(),"application/json") : client.Get(path);
    if (!response) throw std::runtime_error("Daemon unavailable: " + httplib::to_string(response.error()));
    const auto result=nlohmann::json::parse(response->body);
    if (response->status != 200) throw ApiError(response->status,result);
    return result;
}
nlohmann::json ApiClient::status() const { return request("GET","/api/v1/status"); }
nlohmann::json ApiClient::alerts() const { return request("GET","/api/v1/alerts"); }
nlohmann::json ApiClient::thermalLimits(const std::string& pci) const {return request("GET","/api/v1/gpus/"+pci+"/thermal-limits");}
nlohmann::json ApiClient::discover() const { return request("POST","/api/v1/discovery",nlohmann::json::object()); }
nlohmann::json ApiClient::config() const { return request("GET","/api/v1/config"); }
nlohmann::json ApiClient::save(const nlohmann::json& config) const { return request("PUT","/api/v1/config",config); }
nlohmann::json ApiClient::claim(const std::string& path) const { return request("POST","/api/v1/controllers/claim",{{"path",path}}); }
nlohmann::json ApiClient::snapshot(const std::string& id) const { return request("GET","/api/v1/controllers/"+id+"/snapshot"); }
nlohmann::json ApiClient::liveStatus(const std::string& id) const {return request("GET","/api/v1/controllers/"+id+"/status");}
nlohmann::json ApiClient::updateSupply(const std::string& id,const nlohmann::json& body) const {
    return request("PUT","/api/v1/controllers/"+id+"/supply",body);
}
nlohmann::json ApiClient::updateAdapterNames(const std::string& id,const nlohmann::json& body) const {
    return request("PUT","/api/v1/controllers/"+id+"/adapter-names",body);
}
nlohmann::json ApiClient::acknowledgeLatched(const std::string& id,const nlohmann::json& body) const {
    return request("POST","/api/v1/controllers/"+id+"/alerts/acknowledge",body);
}
nlohmann::json ApiClient::calibrationProgress(const std::string& id) const {
    return request("GET","/api/v1/controllers/"+id+"/calibration");
}
nlohmann::json ApiClient::startCalibration(const std::string& id, int group, const nlohmann::json& body) const {
    return request("POST","/api/v1/controllers/"+id+"/groups/"+std::to_string(group)+"/calibration/start",body);
}
nlohmann::json ApiClient::abortCalibration(const std::string& id) const {
    return request("POST","/api/v1/controllers/"+id+"/calibration/abort",{{"confirmed",true}});
}
nlohmann::json ApiClient::updateGroup(const std::string& id, int group, const nlohmann::json& body) const {
    return request("PUT","/api/v1/controllers/"+id+"/groups/"+std::to_string(group),body);
}
nlohmann::json ApiClient::setGroupMode(const std::string& id,int group,const nlohmann::json& body) const {
    return request("POST","/api/v1/controllers/"+id+"/groups/"+std::to_string(group)+"/mode",body);
}
}
