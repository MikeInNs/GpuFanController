#pragma once

#include "AlertPublisher.h"
#include "CalibrationManager.h"
#include "ConfigurationStore.h"
#include "FanGroupController.h"
#include "FanPwmController.h"
#include "Ina3221Monitor.h"
#include "SerialProtocol.h"
#include "Tachometer.h"

namespace fc {

class FirmwareApp {
public:
    void setup();
    void loop();

private:
    void handleFrame(const ProtocolFrame& frame, uint32_t nowMs);
    void updateControllers(uint32_t nowMs);
    void updateGlobalFaults(uint32_t nowMs);
    void updateIdentification(uint32_t nowMs);
    void sendHello(uint16_t sequence);
    void sendHeartbeat(uint16_t sequence);
    void sendAlert(const AlertChanges& changes, uint16_t sequence);
    void sendStatus(uint16_t sequence);
    void sendConfiguration(uint16_t sequence);
    void sendCalibration(uint8_t group, uint16_t sequence);
    void sendAck(const ProtocolFrame& frame, AckStatus status);
    bool decodeConfiguration(const ProtocolFrame& frame,
                             ControllerConfig& config);
    uint8_t encodeConfiguration(uint8_t* payload, uint8_t capacity) const;
    void applyConfiguration(uint32_t nowMs);
    bool savePersistentData();
    bool controllerIdIsSet() const;
    bool hostUpdateTimedOut(uint32_t nowMs) const;

    FanPwmController pwm_;
    Tachometer tachometer_;
    Ina3221Monitor ina3221_;
    ConfigurationStore store_;
    SerialProtocol protocol_;
    AlertPublisher alertPublisher_;
    FanGroupController groups_[kGroupCount];
    CalibrationManager calibrationManager_;
    PersistedData persisted_ = {};
    uint16_t globalActiveFaults_ = 0;
    uint16_t globalLatchedFaults_ = 0;
    uint32_t lastHostUpdateMs_ = 0;
    uint16_t pendingHeartbeatSequence_ = 0;
    bool heartbeatResponsePending_ = false;
    uint32_t identifyUntilMs_ = 0;
    uint32_t previousLedToggleMs_ = 0;
    uint16_t telemetrySequence_ = 0;
};

} // namespace fc
