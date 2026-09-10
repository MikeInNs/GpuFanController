#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace fan {
// Explicit, read-only setup metadata. Never part of the periodic sampler.
class GpuThermalLimits {
public:
    nlohmann::json read(const std::string& pci) const;
    static nlohmann::json parseNvidia(const std::string& pci,const std::string& output);
};
}
