#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
namespace fan::settingsControl {
void validateSupply(const nlohmann::json& request);
std::uint8_t clearMask(const nlohmann::json& request);
}
