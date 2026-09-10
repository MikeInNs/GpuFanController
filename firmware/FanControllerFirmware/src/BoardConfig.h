#pragma once

#include <Arduino.h>

namespace fc {

constexpr uint8_t kGroupCount = 2;
constexpr uint8_t kFanCount = 4;
constexpr uint8_t kFansPerGroup = 2;
constexpr uint8_t kCurvePointCount = 4;
constexpr uint8_t kCalibrationPointCount = 9;

constexpr uint8_t kPwmPins[kGroupCount] = {9, 10};
constexpr uint8_t kTachPins[kFanCount] = {2, 3, 4, 5};

// Logical fan ordering. GPU 2 Fan 1 is physically on D5, while Fan 2 is D4.
constexpr uint8_t kGroupTachIndexes[kGroupCount][kFansPerGroup] = {
    {0, 1}, // GPU 1: D2, D3
    {3, 2}  // GPU 2: D5, D4
};

// INA3221 channel 1 is input voltage only. Channels 2 and 3 measure groups.
constexpr uint8_t kInputVoltageInaChannel = 1;
constexpr uint8_t kGroupInaChannels[kGroupCount] = {2, 3};

constexpr uint8_t kTachPulsesPerRevolution = 2;
constexpr uint16_t kPwmTop = 639; // 16 MHz / 640 = 25 kHz.
constexpr uint32_t kSerialBaud = 115200UL;
constexpr uint8_t kBuiltInLedPin = 13;

} // namespace fc
