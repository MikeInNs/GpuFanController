#pragma once

#include <Arduino.h>
#include "BoardConfig.h"

namespace fc {

enum class OperatingMode : uint8_t {
    Disabled = 0,
    Automatic = 1,
    Manual = 2,
    Full = 3,
    Off = 4,
    Calibration = 5,
    Failsafe = 6,
    Alarm = 7
};

enum class RequestedMode : uint8_t {
    Automatic = 0,
    Manual = 1,
    Full = 2,
    Off = 3
};

enum GlobalFault : uint16_t {
    GlobalFaultNone = 0,
    GlobalFaultInaMissing = 1u << 0,
    GlobalFaultInputVoltageLow = 1u << 1,
    GlobalFaultInputVoltageCritical = 1u << 2,
    GlobalFaultInputVoltageHigh = 1u << 3,
    GlobalFaultDefaultConfiguration = 1u << 4,
    GlobalFaultConfigurationInvalid = 1u << 5,
    GlobalFaultHostTemperatureTimeout = 1u << 6
};

enum GroupFault : uint16_t {
    GroupFaultNone = 0,
    GroupFaultTemperatureInvalid = 1u << 0,
    GroupFaultTemperatureWarning = 1u << 1,
    GroupFaultTemperatureCritical = 1u << 2,
    GroupFaultFan1Slow = 1u << 3,
    GroupFaultFan2Slow = 1u << 4,
    GroupFaultCurrentLow = 1u << 5,
    GroupFaultCurrentHigh = 1u << 6,
    GroupFaultCalibrationAborted = 1u << 7
};

struct __attribute__((packed)) CurvePoint {
    int16_t temperatureDeciC;
    uint16_t targetRpm;
};

struct __attribute__((packed)) GroupConfig {
    uint8_t enabled;
    uint8_t expectedFanMask;
    uint8_t curvePointCount;
    uint8_t minimumDutyPercent;
    uint8_t startupDutyPercent;
    uint16_t startupTimeMs;
    int16_t warningTemperatureDeciC;
    int16_t criticalTemperatureDeciC;
    uint8_t rpmLowThresholdPercent;
    uint8_t faultDelaySeconds;
    uint8_t currentDeviationPercent;
    CurvePoint curve[kCurvePointCount];
};

struct __attribute__((packed)) ControllerConfig {
    uint32_t generation;
    uint16_t hostUpdateTimeoutMs;
    uint16_t inputVoltageWarningLowMv;
    uint16_t inputVoltageCriticalLowMv;
    uint16_t inputVoltageHighMv;
    GroupConfig groups[kGroupCount];
};

struct __attribute__((packed)) CalibrationPoint {
    uint8_t dutyPercent;
    uint16_t fanRpm[kFansPerGroup];
    int16_t currentMilliAmps;
    uint16_t voltageMilliVolts;
};

struct __attribute__((packed)) GroupCalibration {
    uint8_t valid;
    uint8_t pointCount;
    uint8_t startDutyPercent[kFansPerGroup];
    uint8_t minimumRunningDutyPercent;
    uint8_t reserved; // 0x80: measured minimum/adaptive points; bits 0..1: verified stop at 0%.
    CalibrationPoint points[kCalibrationPointCount];
};

struct __attribute__((packed)) CalibrationData {
    uint32_t generation;
    GroupCalibration groups[kGroupCount];
};

struct __attribute__((packed)) PersistedData {
    uint8_t controllerId[16];
    ControllerConfig config;
    CalibrationData calibration;
};

struct InaChannelReading {
    uint16_t busMilliVolts;
    int16_t currentMilliAmps;
    bool valid;
};

struct GroupStatus {
    OperatingMode mode;
    int16_t temperatureDeciC;
    uint16_t temperatureAgeMs;
    uint16_t targetRpm;
    uint8_t dutyPercent;
    uint16_t fanRpm[kFansPerGroup];
    uint16_t busMilliVolts;
    int16_t currentMilliAmps;
    uint16_t activeFaults;
    uint16_t latchedFaults;
};

struct CalibrationStatus {
    bool active;
    uint8_t group;
    uint8_t phase;
    uint8_t step;
    uint8_t dutyPercent;
};

} // namespace fc
