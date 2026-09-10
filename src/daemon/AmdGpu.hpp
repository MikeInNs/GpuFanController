#pragma once
#include "GpuTemperatures.hpp"
#include <filesystem>
#include <set>

namespace fan {
// Read-only amdgpu sysfs backend. Paths are injectable only for fixture tests.
class AmdGpu {
public:
    explicit AmdGpu(std::filesystem::path pciRoot="/sys/bus/pci/devices") : root_(std::move(pciRoot)) {}
    bool owns(const std::string& pci) const;
    bool hasNvidia() const;
    nlohmann::json discover() const;
    GpuReadings sample(const std::set<std::string>& addresses) const;
    nlohmann::json thermalLimits(const std::string& pci) const;
private:
    std::filesystem::path edgeSensor(const std::string& pci) const;
    std::filesystem::path root_;
    std::int16_t temperature(const std::string& pci) const;
};
}
