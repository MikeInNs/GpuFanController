#pragma once
#include "GpuTemperatures.hpp"
#include <sys/types.h>

namespace fan {
// One sampler thread owns this persistent, lazily spawned helper.
class NvmlReader {
public:
    explicit NvmlReader(std::string executable="/proc/self/exe",
                        std::chrono::milliseconds timeout=std::chrono::seconds(3));
    ~NvmlReader();
    NvmlReader(const NvmlReader&)=delete;
    NvmlReader& operator=(const NvmlReader&)=delete;
    GpuReadings sample(const std::set<std::string>& addresses);
    void stop() noexcept;
private:
    std::string executable_;
    std::chrono::milliseconds timeout_;
    int socket_=-1;
    pid_t pid_=-1;
    std::uint64_t sequence_=0;
    void start();
    bool reap() noexcept;
};
}
