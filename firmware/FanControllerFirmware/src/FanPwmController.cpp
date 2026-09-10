#include "FanPwmController.h"
#include <avr/interrupt.h>

namespace fc {

void FanPwmController::begin()
{
    pinMode(kPwmPins[0], OUTPUT);
    pinMode(kPwmPins[1], OUTPUT);

    // The external NPN stages invert the signal. LOW here turns each
    // transistor off, leaving the fan PWM input open/high for full speed.
    digitalWrite(kPwmPins[0], LOW);
    digitalWrite(kPwmPins[1], LOW);

    TCCR1A = _BV(WGM11);
    TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);
    ICR1 = kPwmTop;
    TCNT1 = 0;

    setDuty(0, 100);
    setDuty(1, 100);
}

void FanPwmController::setDuty(uint8_t group, uint8_t dutyPercent)
{
    if (group >= kGroupCount) {
        return;
    }

    if (dutyPercent > 100) dutyPercent = 100;

    if (duty_[group] == dutyPercent) {
        return;
    }

    const uint8_t savedSreg = SREG;
    cli();

    if (group == 0) {
        TCCR1A &= ~(_BV(COM1A1) | _BV(COM1A0));

        if (dutyPercent == 100) {
            PORTB &= ~_BV(PB1);
        } else if (dutyPercent == 0) {
            PORTB |= _BV(PB1);
        } else {
            OCR1A = static_cast<uint16_t>(
                (static_cast<uint32_t>(kPwmTop + 1) *
                 (100 - dutyPercent)) /
                100);
            TCCR1A |= _BV(COM1A1);
        }
    } else {
        TCCR1A &= ~(_BV(COM1B1) | _BV(COM1B0));

        if (dutyPercent == 100) {
            PORTB &= ~_BV(PB2);
        } else if (dutyPercent == 0) {
            PORTB |= _BV(PB2);
        } else {
            OCR1B = static_cast<uint16_t>(
                (static_cast<uint32_t>(kPwmTop + 1) *
                 (100 - dutyPercent)) /
                100);
            TCCR1A |= _BV(COM1B1);
        }
    }

    duty_[group] = dutyPercent;
    SREG = savedSreg;
}

uint8_t FanPwmController::duty(uint8_t group) const
{
    return group < kGroupCount ? duty_[group] : 100;
}

} // namespace fc
