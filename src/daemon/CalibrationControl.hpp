#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include "ConfigStore.hpp"

namespace fan::calibration {
class SafetyRefusal : public ConfigConflict {
public:
    explicit SafetyRefusal(nlohmann::json reasons);
    nlohmann::json details;
};
// Host admission checks only. The Nano owns execution and ongoing safety.
void validateRequest(const nlohmann::json& request, bool start);
void requireForwarding(const nlohmann::json& runtime, const std::string& id, int group);
void requireSafeStart(const nlohmann::json& config, const nlohmann::json& state, int group,
    const nlohmann::json& overrides = nlohmann::json::array());
}
