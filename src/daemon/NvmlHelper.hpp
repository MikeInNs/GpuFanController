#pragma once
#include <cstddef>
namespace fan {
inline constexpr int nvmlHelperFd=3;
inline constexpr std::size_t nvmlMessageLimit=16384;
int runNvmlHelper();
}
