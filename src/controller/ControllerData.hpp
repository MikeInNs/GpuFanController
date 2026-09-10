#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <vector>

namespace fan::controller {
using Json = nlohmann::json;
std::vector<std::uint8_t> configurationBytes(const Json& config); // Validation only; UI never sends these bytes.
Json configuration(const std::vector<std::uint8_t>& bytes);
Json calibration(const std::vector<std::uint8_t>& bytes);
Json status(const std::vector<std::uint8_t>& bytes);
// No nominal RPM defaults: null means measurements cannot establish a range.
Json rpmRange(const Json& calibration, const Json& group);
std::vector<std::uint8_t> editSupply(std::vector<std::uint8_t> bytes,const Json& changes);
std::vector<std::uint8_t> editConfiguration(std::vector<std::uint8_t> bytes,
    int group, const Json& changes, const Json& calibration);
}
