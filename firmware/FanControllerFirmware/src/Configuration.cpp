#include "Configuration.h"
#include <string.h>

namespace fc {

namespace {

void setGroupDefaults(GroupConfig& group)
{
    memset(&group, 0, sizeof(group));
    group.enabled = 1;
    group.expectedFanMask = 0x03;
    group.curvePointCount = 4;
    group.minimumDutyPercent = 20;
    group.startupDutyPercent = 100;
    group.startupTimeMs = 1000;
    group.warningTemperatureDeciC = 750;
    group.criticalTemperatureDeciC = 850;
    group.rpmLowThresholdPercent = 60;
    group.faultDelaySeconds = 3;
    group.currentDeviationPercent = 40;
    group.curve[0] = {350, 2400};
    group.curve[1] = {550, 5000};
    group.curve[2] = {700, 9000};
    group.curve[3] = {820, 12500};
}

} // namespace

void Configuration::setDefaults(ControllerConfig& config)
{
    memset(&config, 0, sizeof(config));
    config.generation = 0;
    config.hostUpdateTimeoutMs = 10000;
    config.inputVoltageWarningLowMv = 11000;
    config.inputVoltageCriticalLowMv = 10500;
    config.inputVoltageHighMv = 13200;

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        setGroupDefaults(config.groups[group]);
    }
}

void Configuration::clearCalibration(CalibrationData& calibration)
{
    memset(&calibration, 0, sizeof(calibration));
}

bool Configuration::validate(const ControllerConfig& config)
{
    if (config.hostUpdateTimeoutMs < 5000 ||
        config.hostUpdateTimeoutMs > 30000 ||
        config.inputVoltageCriticalLowMv < 6000 ||
        config.inputVoltageCriticalLowMv > config.inputVoltageWarningLowMv ||
        config.inputVoltageWarningLowMv >= config.inputVoltageHighMv ||
        config.inputVoltageHighMv > 16000) {
        return false;
    }

    for (uint8_t index = 0; index < kGroupCount; ++index) {
        const GroupConfig& group = config.groups[index];

        if (group.enabled > 1 || group.expectedFanMask > 0x03 ||
            (group.enabled && group.expectedFanMask == 0) ||
            group.curvePointCount == 0 ||
            group.curvePointCount > kCurvePointCount ||
            group.minimumDutyPercent < 1 ||
            group.minimumDutyPercent > 100 ||
            group.startupDutyPercent < group.minimumDutyPercent ||
            group.startupDutyPercent > 100 || group.startupTimeMs > 10000 ||
            group.warningTemperatureDeciC >= group.criticalTemperatureDeciC ||
            group.criticalTemperatureDeciC > 1200 ||
            group.rpmLowThresholdPercent < 20 ||
            group.rpmLowThresholdPercent > 95 ||
            group.faultDelaySeconds == 0 || group.faultDelaySeconds > 30 ||
            group.currentDeviationPercent < 10 ||
            group.currentDeviationPercent > 100) {
            return false;
        }

        for (uint8_t point = 0; point < group.curvePointCount; ++point) {
            if (group.curve[point].targetRpm > 30000) {
                return false;
            }

            if (point > 0 &&
                (group.curve[point].temperatureDeciC <=
                     group.curve[point - 1].temperatureDeciC ||
                 group.curve[point].targetRpm <
                     group.curve[point - 1].targetRpm)) {
                return false;
            }
        }
    }

    return true;
}

void Configuration::apply(PersistedData& data, const ControllerConfig& candidate)
{
    bool topologyChanged = false;
    for (uint8_t group = 0; group < kGroupCount; ++group) {
        if (data.config.groups[group].expectedFanMask != candidate.groups[group].expectedFanMask) {
            // Pair current and startup/RPM data must not survive a different fan topology.
            memset(&data.calibration.groups[group], 0, sizeof(GroupCalibration));
            topologyChanged = true;
        }
    }
    if (topologyChanged) ++data.calibration.generation;
    data.config = candidate;
}

uint16_t Configuration::targetRpm(const GroupConfig& group,
                                  int16_t temperatureDeciC)
{
    if (temperatureDeciC <= group.curve[0].temperatureDeciC) {
        return group.curve[0].targetRpm;
    }

    const uint8_t last = group.curvePointCount - 1;

    if (temperatureDeciC >= group.curve[last].temperatureDeciC) {
        return group.curve[last].targetRpm;
    }

    for (uint8_t point = 1; point < group.curvePointCount; ++point) {
        const CurvePoint& upper = group.curve[point];

        if (temperatureDeciC <= upper.temperatureDeciC) {
            const CurvePoint& lower = group.curve[point - 1];
            const int32_t temperatureRange =
                upper.temperatureDeciC - lower.temperatureDeciC;
            const int32_t temperatureOffset =
                temperatureDeciC - lower.temperatureDeciC;
            const int32_t rpmRange = upper.targetRpm - lower.targetRpm;

            return lower.targetRpm +
                   static_cast<uint16_t>(rpmRange * temperatureOffset /
                                         temperatureRange);
        }
    }

    return group.curve[last].targetRpm;
}

} // namespace fc
