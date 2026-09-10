#include "FirmwareApp.h"
#include "Configuration.h"
#include "AdapterNames.h"
#include <string.h>

namespace fc {

namespace {

constexpr uint8_t kFirmwareMajor = 1;
constexpr uint8_t kFirmwareMinor = 7;
constexpr uint8_t kFirmwarePatch = 0;
constexpr uint16_t kCapabilities = 0x01FF; // Includes persistent adapter display names.

bool timeReached(uint32_t nowMs, uint32_t targetMs)
{
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

} // namespace

void FirmwareApp::setup()
{
    pwm_.begin();
    pinMode(kBuiltInLedPin, OUTPUT);
    digitalWrite(kBuiltInLedPin, LOW);
    Serial.begin(kSerialBaud);
    tachometer_.begin();
    ina3221_.begin();

    store_.load(persisted_);
    if (!Configuration::validate(persisted_.config)) {
        memset(&persisted_, 0, sizeof(persisted_));
        Configuration::setDefaults(persisted_.config);
        Configuration::clearCalibration(persisted_.calibration);
        globalLatchedFaults_ |= GlobalFaultConfigurationInvalid;
    }

    calibrationManager_.begin(&persisted_.calibration, &persisted_.config);
    const uint32_t nowMs = millis();
    for (uint8_t group = 0; group < kGroupCount; ++group) {
        groups_[group].begin(group, &persisted_.config.groups[group],
                             &persisted_.calibration.groups[group], nowMs);
    }
    lastHostUpdateMs_ = nowMs;
}

void FirmwareApp::loop()
{
    const uint32_t nowMs = millis();
    tachometer_.update(nowMs);
    ina3221_.update(nowMs);

    ProtocolFrame frame;
    while (protocol_.poll(Serial, nowMs, frame)) {
        handleFrame(frame, nowMs);
    }

    updateControllers(nowMs);
    updateGlobalFaults(nowMs);
    updateIdentification(nowMs);

    uint16_t groupFaults[kGroupCount];
    for (uint8_t group = 0; group < kGroupCount; ++group)
        groupFaults[group] = groups_[group].status().activeFaults;

    AlertChanges alertChanges;
    if (alertPublisher_.update(globalActiveFaults_, groupFaults,
                               alertChanges)) {
        sendAlert(alertChanges, ++telemetrySequence_);
    }

    if (heartbeatResponsePending_) {
        heartbeatResponsePending_ = false;
        sendHeartbeat(pendingHeartbeatSequence_);
    }
}

void FirmwareApp::handleFrame(const ProtocolFrame& frame, uint32_t nowMs)
{
    switch (frame.type) {
        case MessageHello:
            if (frame.length) {
                sendAck(frame, AckInvalidLength);
            } else {
                sendHello(frame.sequence);
            }
            return;

        case MessageTemperatures: {
            ByteReader reader(frame.payload, frame.length);
            uint8_t validMask;
            int16_t temperatures[kGroupCount];
            if (!reader.getU8(validMask) ||
                !reader.getI16(temperatures[0]) ||
                !reader.getI16(temperatures[1]) || !reader.finished() ||
                validMask > 0x03) {
                sendAck(frame, AckInvalidValue);
                return;
            }

            for (uint8_t group = 0; group < kGroupCount; ++group) {
                const bool valid = validMask & (1u << group);
                if (valid && (temperatures[group] < -400 ||
                              temperatures[group] > 1250)) {
                    sendAck(frame, AckInvalidValue);
                    return;
                }
            }

            for (uint8_t group = 0; group < kGroupCount; ++group) {
                groups_[group].setTemperature(
                    temperatures[group], validMask & (1u << group), nowMs);
            }
            lastHostUpdateMs_ = nowMs;
            pendingHeartbeatSequence_ = frame.sequence;
            heartbeatResponsePending_ = true;
            return;
        }

        case MessageSetConfiguration: {
            if (calibrationManager_.active()) { sendAck(frame, AckBusy); return; }
            ControllerConfig candidate;
            if (!decodeConfiguration(frame, candidate) ||
                !Configuration::validate(candidate)) {
                sendAck(frame, AckInvalidValue);
                return;
            }
            Configuration::apply(persisted_, candidate);
            applyConfiguration(nowMs);
            sendAck(frame, savePersistentData() ? AckOk : AckStorageFailure);
            return;
        }

        case MessageGetConfiguration:
            if (frame.length) sendAck(frame, AckInvalidLength);
            else sendConfiguration(frame.sequence);
            return;

        case MessageGetStatus:
            if (frame.length) sendAck(frame, AckInvalidLength);
            else sendStatus(frame.sequence);
            return;

        case MessageSetMode: {
            if (calibrationManager_.active()) { sendAck(frame, AckBusy); return; }
            ByteReader reader(frame.payload, frame.length);
            uint8_t group;
            uint8_t mode;
            uint8_t duty;
            uint16_t timeout;
            if (!reader.getU8(group) || !reader.getU8(mode) ||
                !reader.getU8(duty) || !reader.getU16(timeout) ||
                !reader.finished() || group >= kGroupCount ||
                mode > static_cast<uint8_t>(RequestedMode::Off) || duty > 100 ||
                timeout > 3600) {
                sendAck(frame, AckInvalidValue);
                return;
            }
            if (!persisted_.config.groups[group].enabled) { sendAck(frame, AckUnsafe); return; }
            groups_[group].setOverride(static_cast<RequestedMode>(mode), duty,
                                       timeout, nowMs);
            sendAck(frame, AckOk);
            return;
        }

        case MessageStartCalibration: {
            if (frame.length != 1 || frame.payload[0] >= kGroupCount) {
                sendAck(frame, AckInvalidValue);
                return;
            }
            const uint8_t group = frame.payload[0];
            const GroupStatus& status = groups_[group].status();
            if (calibrationManager_.active()) {
                sendAck(frame, AckBusy);
            } else if (!ina3221_.present() ||
                       (status.activeFaults & GroupFaultTemperatureInvalid) ||
                       status.temperatureDeciC >=
                           persisted_.config.groups[group]
                               .warningTemperatureDeciC) {
                sendAck(frame, AckUnsafe);
            } else {
                const bool started = calibrationManager_.start(
                    group, persisted_.config.groups[group], nowMs);
                sendAck(frame, started ? AckOk : AckInvalidValue);
            }
            return;
        }

        case MessageAbortCalibration:
            if (frame.length) {
                sendAck(frame, AckInvalidLength);
            } else {
                calibrationManager_.abort();
                sendAck(frame, AckOk);
            }
            return;

        case MessageSetControllerId:
            if (frame.length != sizeof(persisted_.controllerId)) {
                sendAck(frame, AckInvalidLength);
                return;
            }
            {
                bool anyNonzero = false;
                for (uint8_t index = 0; index < frame.length; ++index)
                    anyNonzero |= frame.payload[index] != 0;
                if (!anyNonzero) {
                    sendAck(frame, AckInvalidValue);
                    return;
                }
            }
            memcpy(persisted_.controllerId, frame.payload,
                   sizeof(persisted_.controllerId));
            sendAck(frame, savePersistentData() ? AckOk : AckStorageFailure);
            return;

        case MessageClearFaults: {
            if (frame.length != 1) {
                sendAck(frame, AckInvalidLength);
                return;
            }
            const uint8_t mask = frame.payload[0];
            if (mask & 0x01) groups_[0].clearLatchedFaults();
            if (mask & 0x02) groups_[1].clearLatchedFaults();
            if (mask & 0x80) globalLatchedFaults_ = 0;
            sendAck(frame, AckOk);
            return;
        }

        case MessageIdentify: {
            ByteReader reader(frame.payload, frame.length);
            uint16_t seconds;
            if (!reader.getU16(seconds) || !reader.finished() || seconds > 60) {
                sendAck(frame, AckInvalidValue);
                return;
            }
            identifyUntilMs_ = nowMs + static_cast<uint32_t>(seconds) * 1000UL;
            sendAck(frame, AckOk);
            return;
        }

        case MessageGetCalibration:
            if (frame.length != 1 || frame.payload[0] >= kGroupCount)
                sendAck(frame, AckInvalidValue);
            else
                sendCalibration(frame.payload[0], frame.sequence);
            return;

        case MessageGetAdapterNames: {
            if(frame.length) {sendAck(frame,AckInvalidLength);return;}
            AdapterNames names;AdapterNameStore::load(persisted_.controllerId,names);
            protocol_.send(Serial,MessageAdapterNames,frame.sequence,
                reinterpret_cast<const uint8_t*>(&names),sizeof(names));
            return;
        }
        case MessageSetAdapterNames: {
            if(frame.length!=sizeof(AdapterNames)) {sendAck(frame,AckInvalidLength);return;}
            if(calibrationManager_.active()) {sendAck(frame,AckBusy);return;}
            if(!controllerIdIsSet()) {sendAck(frame,AckUnsafe);return;}
            AdapterNames names;memcpy(&names,frame.payload,sizeof(names));
            sendAck(frame,static_cast<AckStatus>(AdapterNameStore::save(persisted_.controllerId,names)));
            return;
        }
        default:
            sendAck(frame, AckUnsupported);
            return;
    }
}

void FirmwareApp::updateControllers(uint32_t nowMs)
{
    const bool calibrationWasActive = calibrationManager_.active();
    const uint8_t calibrationGroup = calibrationManager_.group();
    const bool hostAlarm = hostUpdateTimedOut(nowMs);

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        groups_[group].setCalibrationOverride(
            calibrationWasActive && group == calibrationGroup,
            calibrationManager_.requestedDuty());

        const uint8_t fan1Index = kGroupTachIndexes[group][0];
        const uint8_t fan2Index = kGroupTachIndexes[group][1];
        groups_[group].update(
            nowMs, hostAlarm, tachometer_.rpm(fan1Index),
            tachometer_.rpm(fan2Index),
            ina3221_.channel(kGroupInaChannels[group]));
    }

    if (calibrationWasActive) {
        const GroupStatus& groupStatus = groups_[calibrationGroup].status();
        const GroupConfig& groupConfig =
            persisted_.config.groups[calibrationGroup];

        if (hostAlarm ||
            (groupStatus.activeFaults & GroupFaultTemperatureInvalid) ||
            groupStatus.temperatureDeciC >=
                groupConfig.warningTemperatureDeciC) {
            calibrationManager_.abort();
        } else {
            calibrationManager_.update(
                nowMs, groupStatus.fanRpm[0], groupStatus.fanRpm[1],
                ina3221_.channel(kGroupInaChannels[calibrationGroup]));
        }
    }

    if (calibrationManager_.consumeDataChanged()) {
        // Restore airflow BEFORE the blocking EEPROM write/verification.
        pwm_.setDuty(calibrationGroup, 100);
        if (calibrationManager_.prepareCommit())
            calibrationManager_.finishCommit(savePersistentData());
        nowMs = millis(); // Start the boost after EEPROM latency, not before it.
        applyConfiguration(nowMs);
    }

    const bool aborted = calibrationManager_.consumeAborted();
    const bool ended = aborted || (calibrationWasActive && !calibrationManager_.active());
    if (ended) {
        groups_[calibrationGroup].setCalibrationOverride(false, 100);
        groups_[calibrationGroup].setOverride(RequestedMode::Automatic, 0, 0, nowMs);
        if (aborted) groups_[calibrationGroup].setCalibrationAbortedFault();
    }

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        uint8_t requestedDuty = groups_[group].status().dutyPercent;
        if (calibrationManager_.active() &&
            group == calibrationManager_.group())
            requestedDuty = calibrationManager_.requestedDuty();
        if (ended && group == calibrationGroup) requestedDuty = 100;
        pwm_.setDuty(group, requestedDuty);
        groups_[group].synchronizeAppliedDuty(requestedDuty, nowMs);
    }
}

void FirmwareApp::updateGlobalFaults(uint32_t nowMs)
{
    globalActiveFaults_ = 0;
    if (!ina3221_.present()) {
        globalActiveFaults_ |= GlobalFaultInaMissing;
    } else {
        const InaChannelReading& input =
            ina3221_.channel(kInputVoltageInaChannel);
        if (input.valid) {
            if (input.busMilliVolts <
                persisted_.config.inputVoltageCriticalLowMv)
                globalActiveFaults_ |= GlobalFaultInputVoltageCritical;
            else if (input.busMilliVolts <
                     persisted_.config.inputVoltageWarningLowMv)
                globalActiveFaults_ |= GlobalFaultInputVoltageLow;
            if (input.busMilliVolts > persisted_.config.inputVoltageHighMv)
                globalActiveFaults_ |= GlobalFaultInputVoltageHigh;
        }
    }

    if (persisted_.config.generation == 0)
        globalActiveFaults_ |= GlobalFaultDefaultConfiguration;
    if (hostUpdateTimedOut(nowMs))
        globalActiveFaults_ |= GlobalFaultHostTemperatureTimeout;
    globalLatchedFaults_ |= globalActiveFaults_;
}

void FirmwareApp::updateIdentification(uint32_t nowMs)
{
    if (identifyUntilMs_ && !timeReached(nowMs, identifyUntilMs_)) {
        if (nowMs - previousLedToggleMs_ >= 250) {
            previousLedToggleMs_ = nowMs;
            digitalWrite(kBuiltInLedPin, !digitalRead(kBuiltInLedPin));
        }
    } else {
        identifyUntilMs_ = 0;
        digitalWrite(kBuiltInLedPin, LOW);
    }
}

void FirmwareApp::sendHello(uint16_t sequence)
{
    uint8_t payload[40];
    ByteWriter writer(payload, sizeof(payload));
    writer.putBytes(persisted_.controllerId, sizeof(persisted_.controllerId));
    writer.putU8(kFirmwareMajor);
    writer.putU8(kFirmwareMinor);
    writer.putU8(kFirmwarePatch);
    writer.putU8(SerialProtocol::kVersion);
    writer.putU16(kCapabilities);
    writer.putU32(persisted_.config.generation);
    writer.putU32(persisted_.calibration.generation);
    writer.putU8(controllerIdIsSet() ? 1 : 0);
    writer.putU8(ina3221_.address());
    protocol_.send(Serial, MessageHelloResponse, sequence, payload,
                   writer.length());
}

void FirmwareApp::sendHeartbeat(uint16_t sequence)
{
    uint8_t payload[24];
    ByteWriter writer(payload, sizeof(payload));
    writer.putU32(millis());
    writer.putU16(globalActiveFaults_);
    for (uint8_t group = 0; group < kGroupCount; ++group)
        writer.putU16(groups_[group].status().activeFaults);
    for (uint8_t group = 0; group < kGroupCount; ++group)
        writer.putU8(static_cast<uint8_t>(groups_[group].status().mode));
    writer.putU32(persisted_.config.generation);
    writer.putU32(persisted_.calibration.generation);
    writer.putU8(ina3221_.present() ? 1 : 0);
    protocol_.send(Serial, MessageHeartbeat, sequence, payload,
                   writer.length());
}

void FirmwareApp::sendAlert(const AlertChanges& changes, uint16_t sequence)
{
    uint8_t payload[18];
    ByteWriter writer(payload, sizeof(payload));
    writer.putU16(changes.globalRaised);
    writer.putU16(changes.globalCleared);
    writer.putU16(changes.globalActive);
    for (uint8_t group = 0; group < kGroupCount; ++group) {
        writer.putU16(changes.groupRaised[group]);
        writer.putU16(changes.groupCleared[group]);
        writer.putU16(changes.groupActive[group]);
    }
    protocol_.send(Serial, MessageAlert, sequence, payload, writer.length());
}

void FirmwareApp::sendStatus(uint16_t sequence)
{
    uint8_t payload[64];
    ByteWriter writer(payload, sizeof(payload));
    writer.putU32(millis());
    const uint32_t hostUpdateAge = millis() - lastHostUpdateMs_;
    writer.putU16(hostUpdateAge > 65535UL
                      ? 65535u
                      : static_cast<uint16_t>(hostUpdateAge));
    writer.putU16(globalActiveFaults_);
    writer.putU16(globalLatchedFaults_);
    writer.putU8(ina3221_.present() ? 1 : 0);
    writer.putU8(ina3221_.address());
    writer.putU8((ina3221_.sdaHigh() ? 1 : 0) |
                 (ina3221_.sclHigh() ? 2 : 0));

    const CalibrationStatus calibration = calibrationManager_.status();
    writer.putU8(calibration.active ? 1 : 0);
    writer.putU8(calibration.group);
    writer.putU8(calibration.phase);
    writer.putU8(calibration.step);
    writer.putU8(calibration.dutyPercent);

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        const GroupStatus& status = groups_[group].status();
        writer.putU8(static_cast<uint8_t>(status.mode));
        writer.putI16(status.temperatureDeciC);
        writer.putU16(status.temperatureAgeMs);
        writer.putU16(status.targetRpm);
        writer.putU8(status.dutyPercent);
        writer.putU16(status.fanRpm[0]);
        writer.putU16(status.fanRpm[1]);
        writer.putU16(status.busMilliVolts);
        writer.putI16(status.currentMilliAmps);
        writer.putU16(status.activeFaults);
        writer.putU16(status.latchedFaults);
    }

    protocol_.send(Serial, MessageStatus, sequence, payload, writer.length());
}

void FirmwareApp::sendConfiguration(uint16_t sequence)
{
    uint8_t payload[ProtocolFrame::kMaximumPayload];
    const uint8_t length = encodeConfiguration(payload, sizeof(payload));
    protocol_.send(Serial, MessageConfiguration, sequence, payload, length);
}

void FirmwareApp::sendCalibration(uint8_t group, uint16_t sequence)
{
    uint8_t payload[ProtocolFrame::kMaximumPayload];
    ByteWriter writer(payload, sizeof(payload));
    const GroupCalibration& calibration = persisted_.calibration.groups[group];
    writer.putU8(group);
    writer.putU8(!calibration.valid ? 0 : (calibration.reserved & 0x80) ?
        3 | ((calibration.reserved & 3) << 2) : 1);
    writer.putU8(calibration.pointCount);
    writer.putU8(calibration.startDutyPercent[0]);
    writer.putU8(calibration.startDutyPercent[1]);
    writer.putU8(calibration.minimumRunningDutyPercent);
    writer.putU32(persisted_.calibration.generation);

    for (uint8_t index = 0; index < kCalibrationPointCount; ++index) {
        const CalibrationPoint& point = calibration.points[index];
        writer.putU8(point.dutyPercent);
        writer.putU16(point.fanRpm[0]);
        writer.putU16(point.fanRpm[1]);
        writer.putI16(point.currentMilliAmps);
        writer.putU16(point.voltageMilliVolts);
    }

    protocol_.send(Serial, MessageCalibration, sequence, payload,
                   writer.length());
}

void FirmwareApp::sendAck(const ProtocolFrame& frame, AckStatus status)
{
    const uint8_t payload[] = {frame.type, static_cast<uint8_t>(status)};
    protocol_.send(Serial, MessageAck, frame.sequence, payload,
                   sizeof(payload));
}

bool FirmwareApp::decodeConfiguration(const ProtocolFrame& frame,
                                      ControllerConfig& config)
{
    memset(&config, 0, sizeof(config));
    ByteReader reader(frame.payload, frame.length);
    if (!reader.getU32(config.generation) ||
        !reader.getU16(config.hostUpdateTimeoutMs) ||
        !reader.getU16(config.inputVoltageWarningLowMv) ||
        !reader.getU16(config.inputVoltageCriticalLowMv) ||
        !reader.getU16(config.inputVoltageHighMv)) return false;

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        GroupConfig& item = config.groups[group];
        if (!reader.getU8(item.enabled) ||
            !reader.getU8(item.expectedFanMask) ||
            !reader.getU8(item.curvePointCount) ||
            !reader.getU8(item.minimumDutyPercent) ||
            !reader.getU8(item.startupDutyPercent) ||
            !reader.getU16(item.startupTimeMs) ||
            !reader.getI16(item.warningTemperatureDeciC) ||
            !reader.getI16(item.criticalTemperatureDeciC) ||
            !reader.getU8(item.rpmLowThresholdPercent) ||
            !reader.getU8(item.faultDelaySeconds) ||
            !reader.getU8(item.currentDeviationPercent)) return false;

        for (uint8_t point = 0; point < kCurvePointCount; ++point) {
            if (!reader.getI16(item.curve[point].temperatureDeciC) ||
                !reader.getU16(item.curve[point].targetRpm)) return false;
        }
    }

    return reader.finished();
}

uint8_t FirmwareApp::encodeConfiguration(uint8_t* payload,
                                         uint8_t capacity) const
{
    ByteWriter writer(payload, capacity);
    const ControllerConfig& config = persisted_.config;
    writer.putU32(config.generation);
    writer.putU16(config.hostUpdateTimeoutMs);
    writer.putU16(config.inputVoltageWarningLowMv);
    writer.putU16(config.inputVoltageCriticalLowMv);
    writer.putU16(config.inputVoltageHighMv);

    for (uint8_t group = 0; group < kGroupCount; ++group) {
        const GroupConfig& item = config.groups[group];
        writer.putU8(item.enabled);
        writer.putU8(item.expectedFanMask);
        writer.putU8(item.curvePointCount);
        writer.putU8(item.minimumDutyPercent);
        writer.putU8(item.startupDutyPercent);
        writer.putU16(item.startupTimeMs);
        writer.putI16(item.warningTemperatureDeciC);
        writer.putI16(item.criticalTemperatureDeciC);
        writer.putU8(item.rpmLowThresholdPercent);
        writer.putU8(item.faultDelaySeconds);
        writer.putU8(item.currentDeviationPercent);
        for (uint8_t point = 0; point < kCurvePointCount; ++point) {
            writer.putI16(item.curve[point].temperatureDeciC);
            writer.putU16(item.curve[point].targetRpm);
        }
    }

    return writer.valid() ? writer.length() : 0;
}

void FirmwareApp::applyConfiguration(uint32_t nowMs)
{
    for (uint8_t group = 0; group < kGroupCount; ++group) {
        groups_[group].applyConfiguration(
            &persisted_.config.groups[group],
            &persisted_.calibration.groups[group], nowMs);
    }
}

bool FirmwareApp::savePersistentData()
{
    return store_.save(persisted_);
}

bool FirmwareApp::controllerIdIsSet() const
{
    for (uint8_t index = 0; index < sizeof(persisted_.controllerId); ++index) {
        if (persisted_.controllerId[index]) return true;
    }
    return false;
}

bool FirmwareApp::hostUpdateTimedOut(uint32_t nowMs) const
{
    return nowMs - lastHostUpdateMs_ >=
           persisted_.config.hostUpdateTimeoutMs;
}

} // namespace fc
