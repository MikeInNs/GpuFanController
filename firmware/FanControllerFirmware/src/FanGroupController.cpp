#include "FanGroupController.h"
#include <stdlib.h>

namespace fc {

namespace {

bool timeReached(uint32_t nowMs, uint32_t targetMs)
{
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

uint8_t clampDuty(int16_t duty)
{
    if (duty < 0) return 0;
    if (duty > 100) return 100;
    return static_cast<uint8_t>(duty);
}

} // namespace

void FanGroupController::begin(uint8_t groupIndex, const GroupConfig* config,
                               const GroupCalibration* calibration,
                               uint32_t nowMs)
{
    groupIndex_ = groupIndex;
    status_.mode = OperatingMode::Failsafe;
    status_.dutyPercent = 100;
    ramp_.reset(100, nowMs);
    applyConfiguration(config, calibration, nowMs);
}

void FanGroupController::applyConfiguration(
    const GroupConfig* config, const GroupCalibration* calibration,
    uint32_t nowMs)
{
    // The persisted config is updated in place; dereferencing config_ here would
    // read the NEW enabled value, not the preceding one.
    const bool controlsChanged = config &&
        (configuredEnabled_ != bool(config->enabled) || configuredFanMask_ != config->expectedFanMask);
    config_ = config;
    calibration_ = calibration;
    rpmTrim_ = 0;

    if (controlsChanged) {
        overrideMode_ = RequestedMode::Automatic;
        overrideUntilMs_ = 0;
        startupUntilMs_ = nowMs + (config_->enabled ? startupTime() : 0);
        fanBadSinceMs_[0] = fanBadSinceMs_[1] = 0;
        currentLowSinceMs_ = currentHighSinceMs_ = 0;
    }
    configuredEnabled_ = config_ && config_->enabled;
    configuredFanMask_ = config_ ? config_->expectedFanMask : 0;
}

void FanGroupController::setTemperature(int16_t temperatureDeciC, bool valid,
                                        uint32_t nowMs)
{
    if (valid != temperatureValid_ ||
        (valid && temperatureDeciC != status_.temperatureDeciC)) {
        lastTemperatureMs_ = nowMs;
    }
    status_.temperatureDeciC = temperatureDeciC;
    temperatureValid_ = valid;
}

void FanGroupController::setOverride(RequestedMode mode, uint8_t dutyPercent,
                                     uint16_t timeoutSeconds, uint32_t nowMs)
{
    overrideMode_ = mode;
    overrideDuty_ = dutyPercent > 100 ? 100 : dutyPercent;
    overrideUntilMs_ =
        mode == RequestedMode::Automatic
            ? 0
            : nowMs + static_cast<uint32_t>(timeoutSeconds ? timeoutSeconds
                                                           : 300) *
                          1000UL;

    if (mode == RequestedMode::Automatic) {
        startupUntilMs_ = nowMs + (config_ ? startupTime() : 1000);
    }
}

void FanGroupController::setCalibrationOverride(bool active,
                                                uint8_t dutyPercent)
{
    calibrationOverride_ = active;
    calibrationDuty_ = dutyPercent > 100 ? 100 : dutyPercent;
}

void FanGroupController::clearLatchedFaults()
{
    status_.latchedFaults = 0;
}

void FanGroupController::synchronizeAppliedDuty(uint8_t dutyPercent, uint32_t nowMs)
{
    // Calibration advances after update(), and completion forces 100% directly.
    // Keep reported PWM and the next Auto ramp anchored to the actual output.
    if (status_.dutyPercent != dutyPercent) {
        status_.dutyPercent = dutyPercent;
        ramp_.reset(dutyPercent, nowMs);
    }
}

void FanGroupController::setCalibrationAbortedFault()
{
    status_.latchedFaults |= GroupFaultCalibrationAborted;
    // One-shot outcome: publish a raised/cleared transition even without a UI
    // status poll. The next normal update clears active, while the latch remains.
    status_.activeFaults |= GroupFaultCalibrationAborted;
}

void FanGroupController::update(uint32_t nowMs, bool hostAlarm,
                                uint16_t fan1Rpm, uint16_t fan2Rpm,
                                const InaChannelReading& power)
{
    const uint16_t previousTargetRpm = status_.targetRpm;
    status_.fanRpm[0] = fan1Rpm;
    status_.fanRpm[1] = fan2Rpm;
    status_.busMilliVolts = power.valid ? power.busMilliVolts : 0;
    status_.currentMilliAmps = power.valid ? power.currentMilliAmps : 0;

    const uint32_t temperatureAge = nowMs - lastTemperatureMs_;
    status_.temperatureAgeMs = temperatureAge > 65535UL
                                   ? 65535u
                                   : static_cast<uint16_t>(temperatureAge);
    status_.activeFaults = 0;

    if (hostAlarm) {
        status_.mode = OperatingMode::Alarm;
        status_.targetRpm = 0;
        status_.dutyPercent = 100;
        ramp_.reset(100, nowMs);
        fanBadSinceMs_[0] = fanBadSinceMs_[1] = 0;
        currentLowSinceMs_ = currentHighSinceMs_ = 0;
        return;
    }

    if (!config_ || !config_->enabled) {
        status_.mode = OperatingMode::Disabled;
        status_.targetRpm = 0;
        status_.dutyPercent = 0;
        ramp_.reset(0, nowMs);
        fanBadSinceMs_[0] = fanBadSinceMs_[1] = 0;
        currentLowSinceMs_ = currentHighSinceMs_ = 0;
        return;
    }

    // The last valid temperature remains authoritative until the daemon sends
    // a changed value or explicitly clears this group's validity bit. Daemon
    // liveness is enforced independently by the heartbeat watchdog.
    const bool temperatureStale = !temperatureValid_;

    if (!temperatureStale) {
        status_.targetRpm =
            Configuration::targetRpm(*config_, status_.temperatureDeciC);

        if (status_.temperatureDeciC >= config_->warningTemperatureDeciC)
            status_.activeFaults |= GroupFaultTemperatureWarning;
        if (status_.temperatureDeciC >= config_->criticalTemperatureDeciC)
            status_.activeFaults |= GroupFaultTemperatureCritical;
    } else {
        status_.targetRpm = 0;
        status_.activeFaults |= GroupFaultTemperatureInvalid;
    }

    if (overrideUntilMs_ && timeReached(nowMs, overrideUntilMs_)) {
        overrideMode_ = RequestedMode::Automatic;
        overrideUntilMs_ = 0;
        startupUntilMs_ = nowMs + startupTime();
    }
    const bool autoStop = overrideMode_ == RequestedMode::Automatic &&
                          status_.targetRpm == 0 && canStop();
    const bool unsupportedStop = !temperatureStale && status_.targetRpm == 0 &&
                                 !canStop() && !measuredMinimum();
    if (!temperatureStale && !autoStop && measuredMinimum() && status_.targetRpm < minimumRpm())
        status_.targetRpm = minimumRpm();
    if (!temperatureStale && !autoStop && overrideMode_ == RequestedMode::Automatic &&
        status_.dutyPercent == 0)
        startupUntilMs_ = nowMs + startupTime();

    if (temperatureStale || unsupportedStop ||
        (status_.activeFaults & GroupFaultTemperatureCritical)) {
        status_.mode = OperatingMode::Failsafe;
        status_.dutyPercent = 100;
    } else if (calibrationOverride_) {
        status_.mode = OperatingMode::Calibration;
        status_.dutyPercent = calibrationDuty_;
    } else if (autoStop) {
        status_.mode = OperatingMode::Automatic;
        status_.dutyPercent = 0;
        rpmTrim_ = 0;
    } else if (overrideMode_ == RequestedMode::Automatic && !timeReached(nowMs, startupUntilMs_)) {
        status_.mode = OperatingMode::Full;
        status_.dutyPercent = startupDuty();
    } else {
        switch (overrideMode_) {
            case RequestedMode::Manual:
                status_.mode = OperatingMode::Manual;
                status_.dutyPercent = overrideDuty_;
                break;
            case RequestedMode::Full:
                status_.mode = OperatingMode::Full;
                status_.dutyPercent = 100;
                break;
            case RequestedMode::Off:
                status_.mode = OperatingMode::Off;
                status_.dutyPercent = 0;
                break;
            case RequestedMode::Automatic:
            default: {
                status_.mode = OperatingMode::Automatic;
                const uint8_t feedForward =
                    feedForwardDuty(status_.targetRpm);

                const bool targetChanged = status_.targetRpm != previousTargetRpm;
                if (targetChanged) rpmTrim_ = 0;
                // Do not accumulate RPM trim against a deliberately ramp-limited output.
                if (!targetChanged && ramp_.settled() && nowMs - previousTrimMs_ >= 1000) {
                    previousTrimMs_ = nowMs;
                    uint16_t measured = 0;

                    for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
                        if (!(config_->expectedFanMask & (1u << fan))) continue;
                        const uint16_t rpm = status_.fanRpm[fan];
                        if (!measured || rpm < measured) measured = rpm;
                    }

                    if (measured) {
                        const int32_t error =
                            static_cast<int32_t>(status_.targetRpm) - measured;
                        if (abs(error) > 150) {
                            int8_t change = constrain(error / 750, -3, 3);
                            if (!change) change = error > 0 ? 1 : -1;
                            rpmTrim_ = constrain(rpmTrim_ + change, -20, 20);
                        }
                    }
                }

                status_.dutyPercent = clampDuty(feedForward + rpmTrim_);
                if (status_.dutyPercent < minimumDuty())
                    status_.dutyPercent = minimumDuty();
                break;
            }
        }
    }

    const uint8_t requestedDuty = status_.dutyPercent;
    if (status_.mode == OperatingMode::Automatic) {
        const uint8_t floor = minimumDuty();
        if (requestedDuty) {
            if (ramp_.duty() < floor) ramp_.reset(floor, nowMs);
            status_.dutyPercent = ramp_.step(requestedDuty, nowMs);
        } else {
            // Decelerate to the safe running floor, then stop without dwelling below it.
            status_.dutyPercent = ramp_.step(floor, nowMs);
            if (status_.dutyPercent <= floor) {
                status_.dutyPercent = 0;
                ramp_.reset(0, nowMs);
            }
        }
    } else {
        // Calibration, startup, overrides and temperature safety never ramp.
        ramp_.reset(status_.dutyPercent, nowMs);
    }

    uint16_t faultReference = status_.targetRpm;
    if ((status_.mode == OperatingMode::Automatic && status_.dutyPercent < requestedDuty) ||
        (status_.mode == OperatingMode::Full && overrideMode_ == RequestedMode::Automatic)) {
        uint16_t reference = expectedRpm(status_.dutyPercent);
        if (!reference) {
            const uint8_t demand = feedForwardDuty(status_.targetRpm);
            const uint32_t scaled = static_cast<uint32_t>(status_.targetRpm) * status_.dutyPercent / (demand ? demand : 100);
            reference = scaled > faultReference ? faultReference : static_cast<uint16_t>(scaled);
        }
        if (reference < faultReference) faultReference = reference;
    }
    status_.activeFaults |= updateFanFaults(nowMs, faultReference);
    updateCurrentFaults(nowMs, power, status_.activeFaults);

    if ((status_.activeFaults &
         (GroupFaultFan1Slow | GroupFaultFan2Slow)) &&
        status_.mode == OperatingMode::Automatic) {
        status_.mode = OperatingMode::Failsafe;
        status_.dutyPercent = 100;
        ramp_.reset(100, nowMs);
    }

    status_.latchedFaults |= status_.activeFaults;
}

const GroupStatus& FanGroupController::status() const
{
    return status_;
}

bool FanGroupController::measuredMinimum() const
{
    return calibration_ && calibration_->valid && (calibration_->reserved & 0x80) &&
           calibration_->pointCount == kCalibrationPointCount;
}
bool FanGroupController::canStop() const
{
    return measuredMinimum() &&
        (calibration_->reserved & config_->expectedFanMask) == config_->expectedFanMask;
}
uint8_t FanGroupController::minimumDuty() const
{
    const uint8_t measured = measuredMinimum() ? calibration_->minimumRunningDutyPercent : 20;
    return config_->minimumDutyPercent > measured ? config_->minimumDutyPercent : measured;
}
uint8_t FanGroupController::startupDuty() const
{
    uint8_t duty = config_->startupDutyPercent;
    if (measuredMinimum()) {
        for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
            if (!(config_->expectedFanMask & (1u << fan))) continue;
            const uint8_t start = calibration_->startDutyPercent[fan];
            const uint8_t safe = start > 97 ? 100 : start + 3;
            if (safe > duty) duty = safe;
        }
    }
    return duty > minimumDuty() ? duty : minimumDuty();
}
uint16_t FanGroupController::startupTime() const
{
    return measuredMinimum() && config_->startupTimeMs < 5000 ? 5000 : config_->startupTimeMs;
}
uint16_t FanGroupController::minimumRpm() const
{
    uint16_t rpm = 30000;
    for (uint8_t i = 0; i < calibration_->pointCount; ++i)
        if (calibration_->points[i].dutyPercent >= minimumDuty()) {
            const uint16_t measured = representativeCalibrationRpm(calibration_->points[i]);
            if (measured < rpm) rpm = measured;
        }
    return rpm;
}

uint8_t FanGroupController::feedForwardDuty(uint16_t targetRpm) const
{
    if (calibration_ && calibration_->valid && calibration_->pointCount >= 2) {
        const CalibrationPoint* lower = nullptr;
        const CalibrationPoint* upper = nullptr;

        for (uint8_t index = 0; index < calibration_->pointCount; ++index) {
            const CalibrationPoint& point = calibration_->points[index];
            const uint16_t rpm = representativeCalibrationRpm(point);
            if (rpm <= targetRpm &&
                (!lower || rpm > representativeCalibrationRpm(*lower)))
                lower = &point;
            if (rpm >= targetRpm &&
                (!upper || rpm < representativeCalibrationRpm(*upper)))
                upper = &point;
        }

        if (!lower) lower = upper;
        if (!upper) upper = lower;

        if (lower && upper) {
            const uint16_t lowerRpm = representativeCalibrationRpm(*lower);
            const uint16_t upperRpm = representativeCalibrationRpm(*upper);
            if (lowerRpm == upperRpm) return lower->dutyPercent;
            const int32_t dutyRange =
                static_cast<int16_t>(upper->dutyPercent) - lower->dutyPercent;
            return clampDuty(lower->dutyPercent +
                             dutyRange * (targetRpm - lowerRpm) /
                                 (upperRpm - lowerRpm));
        }
    }

    const CurvePoint& first = config_->curve[0];
    const CurvePoint& last = config_->curve[config_->curvePointCount - 1];
    if (targetRpm <= first.targetRpm) return minimumDuty();
    if (targetRpm >= last.targetRpm) return 100;
    return minimumDuty() +
           static_cast<uint32_t>(targetRpm - first.targetRpm) *
               (100 - minimumDuty()) /
               (last.targetRpm - first.targetRpm);
}

uint16_t FanGroupController::representativeCalibrationRpm(
    const CalibrationPoint& point) const
{
    uint16_t result = 0;
    for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
        if (!(config_->expectedFanMask & (1u << fan))) continue;
        if (!result || point.fanRpm[fan] < result) result = point.fanRpm[fan];
    }
    return result;
}

int16_t FanGroupController::expectedCurrent(uint8_t dutyPercent) const
{
    if (!calibration_ || !calibration_->valid ||
        calibration_->pointCount < 2) return 0;

    const CalibrationPoint* lower = nullptr;
    const CalibrationPoint* upper = nullptr;
    for (uint8_t index = 0; index < calibration_->pointCount; ++index) {
        const CalibrationPoint& point = calibration_->points[index];
        if (point.dutyPercent <= dutyPercent &&
            (!lower || point.dutyPercent > lower->dutyPercent)) lower = &point;
        if (point.dutyPercent >= dutyPercent &&
            (!upper || point.dutyPercent < upper->dutyPercent)) upper = &point;
    }
    if (!lower) lower = upper;
    if (!upper) upper = lower;
    if (!lower || !upper) return 0;

    const int16_t lowerCurrent = abs(lower->currentMilliAmps);
    const int16_t upperCurrent = abs(upper->currentMilliAmps);
    if (lower->dutyPercent == upper->dutyPercent) return lowerCurrent;
    return lowerCurrent +
           static_cast<int32_t>(upperCurrent - lowerCurrent) *
               (dutyPercent - lower->dutyPercent) /
               (upper->dutyPercent - lower->dutyPercent);
}

uint16_t FanGroupController::expectedRpm(uint8_t dutyPercent) const
{
    if (!calibration_ || !calibration_->valid || calibration_->pointCount < 2) return 0;
    const CalibrationPoint* lower = nullptr;
    const CalibrationPoint* upper = nullptr;
    for (uint8_t i = 0; i < calibration_->pointCount; ++i) {
        const CalibrationPoint& point = calibration_->points[i];
        if (point.dutyPercent <= dutyPercent && (!lower || point.dutyPercent > lower->dutyPercent)) lower = &point;
        if (point.dutyPercent >= dutyPercent && (!upper || point.dutyPercent < upper->dutyPercent)) upper = &point;
    }
    if (!lower) lower = upper;
    if (!upper) upper = lower;
    if (!lower || !upper) return 0;
    const uint16_t rpm = representativeCalibrationRpm(*lower);
    if (lower->dutyPercent == upper->dutyPercent) return rpm;
    return rpm + (static_cast<int32_t>(representativeCalibrationRpm(*upper)) - rpm) *
        (dutyPercent - lower->dutyPercent) / (upper->dutyPercent - lower->dutyPercent);
}

uint16_t FanGroupController::updateFanFaults(uint32_t nowMs, uint16_t referenceRpm)
{
    if (status_.dutyPercent < minimumDuty() ||
        status_.targetRpm == 0 || status_.mode == OperatingMode::Calibration ||
        status_.mode == OperatingMode::Manual ||
        status_.mode == OperatingMode::Off) {
        fanBadSinceMs_[0] = fanBadSinceMs_[1] = 0;
        return 0;
    }

    uint16_t faults = 0;
    const uint16_t minimumRpm =
        static_cast<uint32_t>(referenceRpm) *
        config_->rpmLowThresholdPercent / 100;
    const uint32_t delayMs =
        static_cast<uint32_t>(config_->faultDelaySeconds) * 1000UL;

    for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
        if (!(config_->expectedFanMask & (1u << fan))) {
            fanBadSinceMs_[fan] = 0;
            continue;
        }

        if (status_.fanRpm[fan] < minimumRpm) {
            if (!fanBadSinceMs_[fan]) fanBadSinceMs_[fan] = nowMs;
            if (nowMs - fanBadSinceMs_[fan] >= delayMs)
                faults |= fan == 0 ? GroupFaultFan1Slow : GroupFaultFan2Slow;
        } else {
            fanBadSinceMs_[fan] = 0;
        }
    }

    return faults;
}

void FanGroupController::updateCurrentFaults(
    uint32_t nowMs, const InaChannelReading& power, uint16_t& faults)
{
    if (!power.valid || status_.dutyPercent < minimumDuty() ||
        status_.mode == OperatingMode::Calibration ||
        status_.mode == OperatingMode::Off) {
        currentLowSinceMs_ = currentHighSinceMs_ = 0;
        return;
    }

    const int16_t expected = expectedCurrent(status_.dutyPercent);
    if (expected < 20) return;
    const int16_t actual = abs(power.currentMilliAmps);
    const int16_t low =
        static_cast<int32_t>(expected) *
        (100 - config_->currentDeviationPercent) / 100;
    const int16_t high =
        static_cast<int32_t>(expected) *
        (100 + config_->currentDeviationPercent) / 100;
    const uint32_t delayMs =
        static_cast<uint32_t>(config_->faultDelaySeconds) * 1000UL;

    if (actual < low) {
        if (!currentLowSinceMs_) currentLowSinceMs_ = nowMs;
        if (nowMs - currentLowSinceMs_ >= delayMs)
            faults |= GroupFaultCurrentLow;
    } else {
        currentLowSinceMs_ = 0;
    }

    if (actual > high) {
        if (!currentHighSinceMs_) currentHighSinceMs_ = nowMs;
        if (nowMs - currentHighSinceMs_ >= delayMs)
            faults |= GroupFaultCurrentHigh;
    } else {
        currentHighSinceMs_ = 0;
    }
}

} // namespace fc
