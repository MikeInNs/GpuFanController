#pragma once

#include <Arduino.h>
#include "BoardConfig.h"

namespace fc {

struct AlertChanges {
    uint16_t globalRaised;
    uint16_t globalCleared;
    uint16_t globalActive;
    uint16_t groupRaised[kGroupCount];
    uint16_t groupCleared[kGroupCount];
    uint16_t groupActive[kGroupCount];
};

class AlertPublisher {
public:
    bool update(uint16_t globalActive, const uint16_t* groupActive,
                AlertChanges& changes);

private:
    uint16_t previousGlobalActive_ = 0;
    uint16_t previousGroupActive_[kGroupCount] = {};
};

} // namespace fc
