#pragma once

#include "ControllerTypes.h"

namespace fc {

class Configuration {
public:
    static void setDefaults(ControllerConfig& config);
    static void clearCalibration(CalibrationData& calibration);
    static void apply(PersistedData& data, const ControllerConfig& candidate);
    static bool validate(const ControllerConfig& config);
    static uint16_t targetRpm(const GroupConfig& group, int16_t temperatureDeciC);
};

} // namespace fc
