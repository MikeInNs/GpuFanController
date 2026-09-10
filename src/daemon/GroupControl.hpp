#pragma once
#include <nlohmann/json.hpp>
#include <vector>
#include <cstdint>

namespace fan::groupControl {
void validateMode(const nlohmann::json& request);
std::vector<std::uint8_t> modePayload(int group,const nlohmann::json& request);
void validateEdit(const nlohmann::json& request);
}
