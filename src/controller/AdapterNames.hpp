#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <vector>
namespace fan::adapterNames {
nlohmann::json decode(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> encode(const nlohmann::json& names);
void validateRequest(const nlohmann::json& request);
}
