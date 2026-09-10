#pragma once
#include <map>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

namespace fan {
// Loaded only inside the isolated helper. No CUDA SDK or link-time NVIDIA dependency.
class NvmlLibrary {
public:
    NvmlLibrary() = default;
    ~NvmlLibrary();
    NvmlLibrary(const NvmlLibrary&) = delete;
    NvmlLibrary& operator=(const NvmlLibrary&) = delete;
    nlohmann::json sample(const std::set<std::string>& addresses);
private:
    // Minimal stable C ABI: opaque device pointer and enum arguments/results as int.
    // https://docs.nvidia.com/deploy/nvml-api/api/group__nvmlDeviceQueries.html
    struct Device;
    using Handle=Device*;
    void* library_=nullptr;
    int (*init_)()=nullptr;
    int (*shutdown_)()=nullptr;
    int (*handle_)(const char*,Handle*)=nullptr;
    int (*temperature_)(Handle,int,unsigned*)=nullptr;
    const char* (*error_)(int)=nullptr;
    bool initialized_=false,reinitialize_=false;
    std::map<std::string,Handle> handles_;
    std::map<std::string,unsigned> failures_;
    void initialize();
    std::string failure(const std::string& operation,int code) const;
};
}
