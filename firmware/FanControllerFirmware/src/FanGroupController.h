#pragma once

#include "Configuration.h"
#include "ControllerTypes.h"
#include "PwmRamp.h"

namespace fc {

class FanGroupController {
public:
    void begin(uint8_t groupIndex, const GroupConfig* config,
               const GroupCalibration* calibration, uint32_t nowMs);
    void applyConfiguration(const GroupConfig* config,
                            const GroupCalibration* calibration,
                            uint32_t nowMs);
    void setTemperature(int16_t temperatureDeciC, bool valid,
                        uint32_t nowMs);
    void setOverride(RequestedMode mode, uint8_t dutyPercent,
                     uint16_t timeoutSeconds, uint32_t nowMs);
    void setCalibrationOverride(bool active, uint8_t dutyPercent);
    void synchronizeAppliedDuty(uint8_t dutyPercent, uint32_t nowMs);
    void clearLatchedFaults();
    void setCalibrationAbortedFault();
    void update(uint32_t nowMs, bool hostAlarm, uint16_t fan1Rpm,
                uint16_t fan2Rpm,
                const InaChannelReading& power);

    const GroupStatus& status() const;

private:
    bool measuredMinimum() const;
    bool canStop() const;
    uint8_t minimumDuty() const;
    uint8_t startupDuty() const;
    uint16_t startupTime() const;
    uint16_t minimumRpm() const;
    uint8_t feedForwardDuty(uint16_t targetRpm) const;
    uint16_t representativeCalibrationRpm(const CalibrationPoint& point) const;
    int16_t expectedCurrent(uint8_t dutyPercent) const;
    uint16_t expectedRpm(uint8_t dutyPercent) const;
    uint16_t updateFanFaults(uint32_t nowMs, uint16_t referenceRpm);
    void updateCurrentFaults(uint32_t nowMs, const InaChannelReading& power,
                             uint16_t& faults);

    uint8_t groupIndex_ = 0;
    const GroupConfig* config_ = nullptr;
    bool configuredEnabled_ = false;
    uint8_t configuredFanMask_ = 0;
    const GroupCalibration* calibration_ = nullptr;
    GroupStatus status_ = {};
    PwmRamp ramp_;
    bool temperatureValid_ = false;
    uint32_t lastTemperatureMs_ = 0;
    uint32_t startupUntilMs_ = 0;
    RequestedMode overrideMode_ = RequestedMode::Automatic;
    uint8_t overrideDuty_ = 100;
    uint32_t overrideUntilMs_ = 0;
    bool calibrationOverride_ = false;
    uint8_t calibrationDuty_ = 100;
    int8_t rpmTrim_ = 0;
    uint32_t previousTrimMs_ = 0;
    uint32_t fanBadSinceMs_[kFansPerGroup] = {};
    uint32_t currentLowSinceMs_ = 0;
    uint32_t currentHighSinceMs_ = 0;
};

} // namespace fc
