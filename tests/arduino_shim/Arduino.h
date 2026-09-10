#pragma once
// Native firmware policy tests only; no physical GPIO, USB, timers or EEPROM.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#define constrain(x, low, high) ((x)<(low)?(low):((x)>(high)?(high):(x)))
