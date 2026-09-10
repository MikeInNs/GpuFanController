#pragma once
#include <stdint.h>

namespace fc {
// Integer PWM percentage points; fractional time is retained across fast loops.
// This policy is used by Auto control only, never in the hardware PWM driver.
class PwmRamp {
public:
    void reset(uint8_t duty, uint32_t nowMs) {
        duty_ = target_ = duty > 100 ? 100 : duty;
        lastMs_ = nowMs;
        credit_ = 0;
    }
    uint8_t duty() const { return duty_; }
    bool settled() const { return duty_ == target_; }
    uint8_t step(uint8_t target, uint32_t nowMs) {
        if (target > 100) target = 100;
        uint32_t elapsed = nowMs - lastMs_;
        lastMs_ = nowMs;
        // No banked idle/bypass time, and no old-direction credit on reversal.
        if (target != target_ && (settled() || ((target > duty_) != (target_ > duty_)))) {
            elapsed = 0;
            credit_ = 0;
        }
        target_ = target;
        if (settled()) { credit_ = 0; return duty_; }
        if (elapsed > 40000UL) elapsed = 40000UL; // Bound arithmetic after long gaps.
        const uint8_t rate = target_ > duty_ ? 10 : 3;
        const uint32_t budget = credit_ + elapsed * rate;
        const uint16_t steps = budget / 1000;
        credit_ = budget % 1000;
        const uint8_t distance = target_ > duty_ ? target_ - duty_ : duty_ - target_;
        if (steps >= distance) { duty_ = target_; credit_ = 0; }
        else if (target_ > duty_) duty_ += steps;
        else duty_ -= steps;
        return duty_;
    }
private:
    uint32_t lastMs_ = 0;
    uint16_t credit_ = 0;
    uint8_t duty_ = 100;
    uint8_t target_ = 100;
};
}
