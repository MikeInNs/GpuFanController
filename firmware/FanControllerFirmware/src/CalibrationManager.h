#pragma once

#include "ControllerTypes.h"

namespace fc {

class CalibrationManager {
public:
    enum Phase : uint8_t {
        Idle = 0,
        Sweep = 1,
        SpinDown = 2,
        StartSearch = 3,
        Complete = 4, // Measurements ready; NOT a storage confirmation.
        Aborted = 5,
        Saved = 6,
        StorageFailed = 7,
        Unstable = 8,
        InvalidMeasurements = 9,
        TimedOut = 10,
        PowerUnavailable = 11,
        MinimumSearch = 12,
        RunningBoost = 13,
        MinimumVerify = 14,
        VerifyStop = 15,
        StartVerify = 16
    };

    void begin(CalibrationData* calibration, ControllerConfig* configuration);
    bool start(uint8_t group, const GroupConfig& config, uint32_t nowMs);
    void abort(Phase reason = Aborted);
    void update(uint32_t nowMs, uint16_t fan1Rpm, uint16_t fan2Rpm,
                const InaChannelReading& power);
    bool active() const;
    uint8_t group() const;
    uint8_t requestedDuty() const;
    CalibrationStatus status() const;
    bool consumeDataChanged();
    bool consumeAborted();
    bool prepareCommit();
    void finishCommit(bool saved);

private:
    void recordSweepPoint(uint16_t fan1Rpm, uint16_t fan2Rpm,
                          const InaChannelReading& power);
    void finish();
    void resetWindow(uint32_t nowMs);
    bool sample(uint32_t nowMs, uint16_t fan1Rpm, uint16_t fan2Rpm,
                const InaChannelReading& power, uint16_t settleMs, bool requireRunning);
    void swapWorking();
    void nextPhase(Phase phase, uint8_t duty, uint32_t nowMs);

    CalibrationData* calibration_ = nullptr;
    ControllerConfig* configuration_ = nullptr;
    uint8_t oldMinimum_ = 0, oldStartup_ = 0;
    uint16_t oldStartupMs_ = 0;
    uint8_t trial_ = 0, startedMask_ = 0, lastRunning_ = 100;
    const GroupConfig* config_ = nullptr;
    GroupCalibration working_ = {};
    uint32_t runStartedMs_ = 0;
    uint32_t lastSampleMs_ = 0;
    uint32_t rpmSum_[2] = {};
    int32_t currentSum_ = 0;
    uint32_t voltageSum_ = 0;
    uint16_t firstRpm_[2] = {};
    uint8_t samples_ = 0;
    bool commitPending_ = false;
    uint8_t group_ = 0;
    Phase phase_ = Idle;
    uint8_t step_ = 0;
    uint8_t requestedDuty_ = 100;
    uint32_t phaseStartedMs_ = 0;
    bool dataChanged_ = false;
    bool aborted_ = false;
};

} // namespace fc
