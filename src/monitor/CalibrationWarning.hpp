#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace fan {
struct CalibrationWarning {
    std::string message;
    nlohmann::json overrides=nlohmann::json::array();
    bool canOverride=false;
};
inline CalibrationWarning calibrationWarning(const nlohmann::json& safety) {
    CalibrationWarning result;
    bool allOverridable=true;
    for(const auto& reason:safety.at("reasons")) {
        result.message+="- "+reason.at("message").get<std::string>()+"\n";
        const auto& code=reason.at("code");
        const bool allowed=reason.at("overridable")==true &&
            (code=="power_voltage_low" || code=="power_voltage_high" || code=="power_voltage_unavailable");
        allOverridable&=allowed;
        if(allowed) result.overrides.push_back(code);
    }
    result.canOverride=safety.value("overrideAllowed",false) && allOverridable && !result.overrides.empty();
    if(result.canOverride) result.message+=
        "\nRISK: Calibration reduces/stops cooling. Overriding power warnings can produce invalid RPM/current measurements, overheat the GPU, or damage fans if the supply is wrong. "
        "Proceed only after independently checking fan power, with the GPU idle and under supervision. "
        "This override applies only to this start attempt; Nano temperature and watchdog protections remain active.";
    else result.message+="\nThis condition cannot be overridden. Restore safe conditions and Read Nano before trying again. Nano temperature and watchdog protections remain active.";
    return result;
}
}
