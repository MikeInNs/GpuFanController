#include "Tachometer.h"
#include <avr/interrupt.h>

namespace fc {

volatile uint32_t Tachometer::pulseCounts_[kFanCount] = {};
volatile uint8_t Tachometer::previousPortD_ = 0;

void Tachometer::begin()
{
    for (uint8_t index = 0; index < kFanCount; ++index) {
        pinMode(kTachPins[index], INPUT);
    }

    previousPortD_ = PIND;
    PCIFR = _BV(PCIF2);
    PCMSK2 |= _BV(PD2) | _BV(PD3) | _BV(PD4) | _BV(PD5);
    PCICR |= _BV(PCIE2);
    previousSampleMs_ = millis();
}

void Tachometer::handlePinChange()
{
    constexpr uint8_t mask =
        _BV(PD2) | _BV(PD3) | _BV(PD4) | _BV(PD5);
    const uint8_t current = PIND;
    const uint8_t falling = previousPortD_ & ~current & mask;

    if (falling & _BV(PD2)) ++pulseCounts_[0];
    if (falling & _BV(PD3)) ++pulseCounts_[1];
    if (falling & _BV(PD4)) ++pulseCounts_[2];
    if (falling & _BV(PD5)) ++pulseCounts_[3];

    previousPortD_ = current;
}

bool Tachometer::update(uint32_t nowMs)
{
    const uint32_t elapsedMs = nowMs - previousSampleMs_;

    if (elapsedMs < 1000) {
        return false;
    }

    uint32_t currentCounts[kFanCount];
    const uint8_t savedSreg = SREG;
    cli();

    for (uint8_t index = 0; index < kFanCount; ++index) {
        currentCounts[index] = pulseCounts_[index];
    }

    SREG = savedSreg;

    for (uint8_t index = 0; index < kFanCount; ++index) {
        const uint32_t pulses = currentCounts[index] - previousCounts_[index];
        const uint32_t calculated =
            pulses * 60000UL /
            (static_cast<uint32_t>(kTachPulsesPerRevolution) * elapsedMs);
        rpm_[index] = calculated > 65535UL
                          ? 65535u
                          : static_cast<uint16_t>(calculated);
        previousCounts_[index] = currentCounts[index];
    }

    previousSampleMs_ = nowMs;
    return true;
}

uint16_t Tachometer::rpm(uint8_t tachIndex) const
{
    return tachIndex < kFanCount ? rpm_[tachIndex] : 0;
}

} // namespace fc

ISR(PCINT2_vect)
{
    fc::Tachometer::handlePinChange();
}
