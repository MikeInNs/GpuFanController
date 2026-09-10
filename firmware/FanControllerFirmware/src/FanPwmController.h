#pragma once

#include <Arduino.h>
#include "BoardConfig.h"

namespace fc {

class FanPwmController {
public:
    void begin();
    void setDuty(uint8_t group, uint8_t dutyPercent);
    uint8_t duty(uint8_t group) const;

private:
    uint8_t duty_[kGroupCount] = {100, 100};
};

} // namespace fc
