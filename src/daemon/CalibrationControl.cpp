#include "CalibrationControl.hpp"
#include "ConfigStore.hpp"
#include <stdexcept>
#include <algorithm>
#include <set>

namespace fan::calibration {
using Json=nlohmann::json;
namespace {
bool powerWarning(const Json& code) {
    return code=="power_voltage_low" || code=="power_voltage_high" || code=="power_voltage_unavailable";
}
Json reason(const char* code, const std::string& message, bool overridable=false) {
    return {{"code",code},{"message",message},{"overridable",overridable}};
}
}
SafetyRefusal::SafetyRefusal(Json reasons):ConfigConflict("Calibration blocked: "+reasons.at(0).at("message").get<std::string>()) {
    const bool allowed=std::all_of(reasons.begin(),reasons.end(),[](const auto& r){return r.at("overridable")==true;});
    details={{"reasons",std::move(reasons)},{"overrideAllowed",allowed}};
}
void validateRequest(const nlohmann::json& request, bool start) {
    if(!request.is_object() || !request.contains("confirmed") || request["confirmed"]!=true ||
       request.size()!=(start?(request.contains("overrideWarnings")?4u:3u):1u))
        throw std::invalid_argument("Calibration requires explicit confirmation and the documented request fields");
    if(start) for(const auto* key:{"configGeneration","calibrationGeneration"}) {
        if(!request.contains(key) || !request[key].is_number_integer() ||
           request[key].get<std::int64_t>()<0 || request[key].get<std::uint64_t>()>0xffffffffu)
            throw std::invalid_argument("Calibration requires valid configuration/calibration generations");
    }
    if(start && request.contains("overrideWarnings")) {
        const auto& codes=request.at("overrideWarnings");
        if(!codes.is_array() || codes.size()>3) throw std::invalid_argument("overrideWarnings must list acknowledged power warning codes");
        std::set<std::string> seen;
        for(const auto& code:codes) if(!code.is_string() || !powerWarning(code) || !seen.insert(code.get<std::string>()).second)
            throw std::invalid_argument("Unknown, mandatory or duplicate calibration warning override");
    }
}
void requireForwarding(const nlohmann::json& runtime, const std::string& id, int group) {
    if(runtime.value("enabled",false)) for(const auto& worker:runtime.at("controllers")) {
        if(worker.at("controllerId")!=id) continue;
        const int bit=1<<group;
        if(worker.value("responsive",false) && !worker.at("lastReplyAgeMs").is_null() &&
           worker.at("lastReplyAgeMs").get<int>()<6500 &&
           (worker.value("requiredValidMask",0)&bit) && (worker.value("validMask",0)&bit)) return;
    }
    throw SafetyRefusal(Json::array({reason("temperature_forwarding_unavailable",
        "A saved GPU mapping and recent valid temperature-forwarding replies are required for this group.")}));
}
void requireSafeStart(const nlohmann::json& config, const nlohmann::json& state, int group, const Json& overrides) {
    const auto& settings=config.at("groups").at(group);
    const auto& reading=state.at("groups").at(group);
    Json reasons=Json::array();
    if(state.at("calibrationActive").get<bool>()) reasons.push_back(reason("calibration_active","A calibration is already active on this controller."));
    if(!settings.at("enabled").get<bool>() || settings.at("expectedFanMask").get<int>()<1 ||
       settings.at("expectedFanMask").get<int>()>3)
        reasons.push_back(reason("group_not_configured","Enable the Nano group and configure its expected fans first."));
    const int faults=state.at("activeFaults");
    if(!state.at("inaPresent").get<bool>() || (faults&1)) reasons.push_back(reason("ina_missing","The Nano reports the INA3221 missing."));
    if((faults&64) || state.at("hostUpdateAgeMs").get<int>()>=6500)
        reasons.push_back(reason("host_update_stale","The Nano has not received a recent temperature packet."));
    if(reading.at("temperatureDeciC").is_null()) reasons.push_back(reason("temperature_invalid","The GPU temperature is unavailable."));
    else if(reading.at("temperatureDeciC")>=settings.at("warningTemperatureDeciC"))
        reasons.push_back(reason("temperature_high","The GPU temperature is at or above the Nano warning threshold. Let the GPU cool first."));
    if(reading.at("voltageMv").is_null())
        reasons.push_back(reason("power_voltage_unavailable","The fan-group supply voltage reading is unavailable.",true));
    if((faults&6) || (!reading.at("voltageMv").is_null() && reading.at("voltageMv")<config.at("voltageWarningLowMv")))
        reasons.push_back(reason("power_voltage_low","The supply reports a low/critical-low voltage alert or the fan-group voltage is below its warning limit.",true));
    if((faults&8) || (!reading.at("voltageMv").is_null() && reading.at("voltageMv")>config.at("voltageHighMv")))
        reasons.push_back(reason("power_voltage_high","The supply reports a high-voltage alert or the fan-group voltage exceeds its high limit.",true));
    Json remaining=Json::array();
    for(const auto& r:reasons) {
        // Only named power warnings may be acknowledged. Mandatory checks never disappear.
        const bool acknowledged=r["overridable"]==true && powerWarning(r["code"]) &&
            std::find(overrides.begin(),overrides.end(),r["code"])!=overrides.end();
        if(!acknowledged) remaining.push_back(r);
    }
    if(!remaining.empty()) throw SafetyRefusal(std::move(remaining));
}
}
