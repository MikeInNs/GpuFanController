#include "CalibrationManager.h"
#include <string.h>

namespace fc {

void CalibrationManager::begin(CalibrationData* calibration, ControllerConfig* configuration)
{
    calibration_ = calibration;
    configuration_ = configuration;
}

bool CalibrationManager::start(uint8_t group, const GroupConfig& config,
                               uint32_t nowMs)
{
    if (active() || !calibration_ || !configuration_ || group >= kGroupCount || !config.enabled ||
        !config.expectedFanMask || config.expectedFanMask > 3)
        return false;

    group_ = group;
    config_ = &config;
    memset(&working_, 0, sizeof(working_));
    phase_ = SpinDown;
    step_ = 0;
    requestedDuty_ = 0;
    trial_ = startedMask_ = 0;
    working_.reserved = 0x80 | config.expectedFanMask; // Only test expected fans; clear a bit if either trial cannot stop it.
    runStartedMs_ = nowMs;
    resetWindow(nowMs);
    commitPending_ = false;
    dataChanged_ = false;
    aborted_ = false;
    return true;
}

void CalibrationManager::abort(Phase reason)
{
    if (!active()) return;
    phase_ = reason;
    requestedDuty_ = 100;
    aborted_ = true;
    dataChanged_ = false;
}

void CalibrationManager::update(uint32_t nowMs, uint16_t fan1Rpm,
                                uint16_t fan2Rpm,
                                const InaChannelReading& power)
{
    if (!active()) return;

    if (nowMs - runStartedMs_ >= 720000UL) { abort(TimedOut); return; }
    if (!power.valid || !power.busMilliVolts) { abort(PowerUnavailable); return; }
    GroupCalibration& result = working_;

    if (phase_ == Sweep) {
        if (!sample(nowMs, fan1Rpm, fan2Rpm, power, 8000, true)) return;
        const InaChannelReading average = {static_cast<uint16_t>(voltageSum_ / samples_),
            static_cast<int16_t>(currentSum_ / samples_), true};
        recordSweepPoint(rpmSum_[0] / samples_, rpmSum_[1] / samples_, average);
        ++step_;

        if (step_ < kCalibrationPointCount) {
            requestedDuty_ = 100 - static_cast<uint16_t>(100 - result.minimumRunningDutyPercent) * step_ / 8;
            resetWindow(nowMs);
        } else {
            finish();
        }
        return;
    }

    if (phase_ == SpinDown || phase_ == VerifyStop) {
        if (!sample(nowMs, fan1Rpm, fan2Rpm, power, 8000, false)) return;

        const uint16_t rpms[kFansPerGroup] = {fan1Rpm, fan2Rpm};
        for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
            // Require three actual zero samples, not a final zero after coasting.
            if ((config_->expectedFanMask & (1u << fan)) && rpms[fan] < 300 && rpmSum_[fan] != 0) {
                samples_ = 0;
                return;
            }
        }
        for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
            if ((config_->expectedFanMask & (1u << fan)) && rpms[fan] != 0) {
                if (phase_ == VerifyStop && (result.reserved & (1u << fan))) {
                    abort(InvalidMeasurements); return;
                }
                if (rpms[fan] < 300) { abort(InvalidMeasurements); return; }
                result.reserved &= ~(1u << fan);
                result.startDutyPercent[fan] = 100; // Cannot verify a start from rest: conservative boost.
                startedMask_ |= 1u << fan;
            }
        }

        if (phase_ == VerifyStop) {
            uint8_t duty = 0;
            for (uint8_t fan = 0; fan < kFansPerGroup; ++fan)
                if ((config_->expectedFanMask & (1u << fan)) && result.startDutyPercent[fan] > duty)
                    duty = result.startDutyPercent[fan];
            nextPhase(StartVerify, duty > 97 ? 100 : duty + 3, nowMs);
        } else nextPhase(StartSearch, 5, nowMs);
        return;
    }

    if (phase_ == StartSearch) {
        if (!sample(nowMs, fan1Rpm, fan2Rpm, power, 5000, false)) return;

        const uint16_t rpms[kFansPerGroup] = {fan1Rpm, fan2Rpm};
        for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
            if ((config_->expectedFanMask & (1u << fan)) &&
                !(startedMask_ & (1u << fan)) && rpms[fan] >= 300) {
                startedMask_ |= 1u << fan;
                if (requestedDuty_ > result.startDutyPercent[fan])
                    result.startDutyPercent[fan] = requestedDuty_;
            }
        }

        if ((startedMask_ & config_->expectedFanMask) == config_->expectedFanMask) {
            if (++trial_ < 2) {
                startedMask_ = 0;
                nextPhase(SpinDown, 0, nowMs);
            } else {
                trial_ = 0;
                nextPhase(VerifyStop, 0, nowMs);
            }
        } else if (requestedDuty_ >= 100) {
            abort(InvalidMeasurements);
        } else {
            requestedDuty_ += 5;
            resetWindow(nowMs);
        }
        return;
    }

    if (phase_ == StartVerify) {
        if (!sample(nowMs, fan1Rpm, fan2Rpm, power, 5000, true)) return;
        if (++trial_ < 2) nextPhase(VerifyStop, 0, nowMs);
        else {
            trial_ = 0;
            lastRunning_ = 100;
            nextPhase(MinimumSearch, 100, nowMs);
        }
        return;
    }
    if (phase_ == MinimumSearch) {
        if (!sample(nowMs, fan1Rpm, fan2Rpm, power, 5000, false)) return;
        const bool running = (!(config_->expectedFanMask & 1) || fan1Rpm >= 300) &&
                             (!(config_->expectedFanMask & 2) || fan2Rpm >= 300);
        if (!running || requestedDuty_ == 1) {
            if (running) lastRunning_ = 1;
            // The first fan to stop defines the group's floor. Add three PWM percentage points.
            result.minimumRunningDutyPercent = lastRunning_ + 3;
            if (result.minimumRunningDutyPercent > 92) { abort(InvalidMeasurements); return; }
            nextPhase(RunningBoost, 100, nowMs);
        } else {
            lastRunning_ = requestedDuty_;
            nextPhase(MinimumSearch, requestedDuty_ > 5 ? requestedDuty_ - 5 : 1, nowMs);
        }
        return;
    }
    if (phase_ == RunningBoost) {
        if (sample(nowMs, fan1Rpm, fan2Rpm, power, 5000, true))
            nextPhase(MinimumVerify, result.minimumRunningDutyPercent, nowMs);
        return;
    }
    if (phase_ == MinimumVerify) {
        if (!sample(nowMs, fan1Rpm, fan2Rpm, power, 8000, true)) return;
        if (++trial_ < 2) nextPhase(RunningBoost, 100, nowMs);
        else { step_ = 0; nextPhase(Sweep, 100, nowMs); }
    }
}

void CalibrationManager::nextPhase(Phase phase, uint8_t duty, uint32_t nowMs)
{
    phase_ = phase;
    requestedDuty_ = duty;
    resetWindow(nowMs);
}

bool CalibrationManager::active() const
{
    return phase_ == Sweep || phase_ == SpinDown || phase_ == StartSearch || phase_ == Complete ||
           phase_ == MinimumSearch || phase_ == RunningBoost || phase_ == MinimumVerify ||
           phase_ == VerifyStop || phase_ == StartVerify;
}

uint8_t CalibrationManager::group() const { return group_; }
uint8_t CalibrationManager::requestedDuty() const { return requestedDuty_; }

CalibrationStatus CalibrationManager::status() const
{
    return {active(), group_, static_cast<uint8_t>(phase_), step_,
            requestedDuty_};
}

bool CalibrationManager::consumeDataChanged()
{
    const bool result = dataChanged_;
    dataChanged_ = false;
    return result;
}

bool CalibrationManager::consumeAborted()
{
    const bool result = aborted_;
    aborted_ = false;
    return result;
}

void CalibrationManager::recordSweepPoint(
    uint16_t fan1Rpm, uint16_t fan2Rpm, const InaChannelReading& power)
{
    GroupCalibration& result = working_;
    CalibrationPoint& point = result.points[step_];
    point.dutyPercent = requestedDuty_;
    point.fanRpm[0] = fan1Rpm;
    point.fanRpm[1] = fan2Rpm;
    point.currentMilliAmps = power.valid ? power.currentMilliAmps : 0;
    point.voltageMilliVolts = power.valid ? power.busMilliVolts : 0;
    result.pointCount = step_ + 1;
}

void CalibrationManager::finish()
{
    GroupCalibration& result = working_;
    result.valid = 1;
    phase_ = Complete;
    requestedDuty_ = 100;
    dataChanged_ = true;
}

void CalibrationManager::resetWindow(uint32_t nowMs)
{
    phaseStartedMs_ = lastSampleMs_ = nowMs;
    samples_ = 0;
}

bool CalibrationManager::sample(uint32_t nowMs, uint16_t fan1Rpm, uint16_t fan2Rpm,
                                const InaChannelReading& power, uint16_t settleMs,
                                bool requireRunning)
{
    const uint32_t elapsed = nowMs - phaseStartedMs_;
    const bool missingExpected = ((config_->expectedFanMask & 1) && fan1Rpm < 300) ||
                                 ((config_->expectedFanMask & 2) && fan2Rpm < 300);
    if (elapsed >= 18000) { abort(requireRunning && missingExpected
        ? InvalidMeasurements : Unstable); return false; }
    if (elapsed < settleMs || nowMs - lastSampleMs_ < 1000) return false;
    lastSampleMs_ = nowMs;
    const uint16_t rpm[2] = {fan1Rpm, fan2Rpm};
    bool stable = true;
    for (uint8_t fan = 0; fan < 2; ++fan) {
        if (!(config_->expectedFanMask & (1u << fan))) continue;
        if (requireRunning && rpm[fan] < 300) { samples_ = 0; return false; }
        const uint16_t tolerance = firstRpm_[fan] / 20 > 150 ? firstRpm_[fan] / 20 : 150;
        const uint16_t difference = rpm[fan] > firstRpm_[fan] ? rpm[fan] - firstRpm_[fan] : firstRpm_[fan] - rpm[fan];
        if (samples_ && difference > tolerance) stable = false;
        // Never average a stopped/running transition into a startup verdict.
        if (samples_ && ((rpm[fan] >= 300) != (firstRpm_[fan] >= 300))) stable = false;
    }
    if (!stable) samples_ = 0;
    if (!samples_) {
        rpmSum_[0] = rpmSum_[1] = voltageSum_ = 0;
        currentSum_ = 0;
        firstRpm_[0] = rpm[0];firstRpm_[1] = rpm[1];
    }
    rpmSum_[0] += rpm[0];rpmSum_[1] += rpm[1];
    voltageSum_ += power.busMilliVolts;currentSum_ += power.currentMilliAmps;
    return ++samples_ >= 3;
}

void CalibrationManager::swapWorking()
{
    uint8_t* target = reinterpret_cast<uint8_t*>(&calibration_->groups[group_]);
    uint8_t* working = reinterpret_cast<uint8_t*>(&working_);
    for (uint8_t i = 0; i < sizeof(GroupCalibration); ++i) {
        const uint8_t old = target[i];target[i] = working[i];working[i] = old;
    }
}

bool CalibrationManager::prepareCommit()
{
    if (phase_ != Complete || commitPending_) return false;
    // The working buffer becomes the rollback copy; no second table on the stack.
    swapWorking();++calibration_->generation;commitPending_ = true;
    GroupConfig& config = configuration_->groups[group_];
    oldMinimum_ = config.minimumDutyPercent;
    oldStartup_ = config.startupDutyPercent;
    oldStartupMs_ = config.startupTimeMs;
    const GroupCalibration& measured = calibration_->groups[group_];
    config.minimumDutyPercent = measured.minimumRunningDutyPercent;
    config.startupDutyPercent = config.minimumDutyPercent;
    for (uint8_t fan = 0; fan < kFansPerGroup; ++fan) {
        if (!(config.expectedFanMask & (1u << fan))) continue;
        const uint8_t boosted = measured.startDutyPercent[fan] > 97 ? 100 : measured.startDutyPercent[fan] + 3;
        if (boosted > config.startupDutyPercent) config.startupDutyPercent = boosted;
    }
    config.startupTimeMs = 5000;
    ++configuration_->generation;
    return true;
}

void CalibrationManager::finishCommit(bool saved)
{
    if (!commitPending_) return;
    if (!saved) {
        --calibration_->generation;
        swapWorking();
        GroupConfig& config = configuration_->groups[group_];
        config.minimumDutyPercent = oldMinimum_;
        config.startupDutyPercent = oldStartup_;
        config.startupTimeMs = oldStartupMs_;
        --configuration_->generation;
        aborted_ = true;
    }
    commitPending_ = false;
    phase_ = saved ? Saved : StorageFailed;
    requestedDuty_ = 100;
}

} // namespace fc
