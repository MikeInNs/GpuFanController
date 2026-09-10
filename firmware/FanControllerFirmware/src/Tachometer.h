#pragma once

#include <Arduino.h>
#include "BoardConfig.h"

namespace fc {

class Tachometer {
public:
    void begin();
    bool update(uint32_t nowMs);
    uint16_t rpm(uint8_t tachIndex) const;
    static void handlePinChange();

private:
    static volatile uint32_t pulseCounts_[kFanCount];
    static volatile uint8_t previousPortD_;
    uint32_t previousCounts_[kFanCount] = {};
    uint16_t rpm_[kFanCount] = {};
    uint32_t previousSampleMs_ = 0;
};

} // namespace fc
