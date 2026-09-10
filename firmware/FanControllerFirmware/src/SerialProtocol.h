#pragma once

#include <Arduino.h>

namespace fc {

enum MessageType : uint8_t {
    MessageHello = 0x01,
    MessageTemperatures = 0x02,
    MessageSetConfiguration = 0x03,
    MessageGetConfiguration = 0x04,
    MessageGetStatus = 0x05,
    MessageSetMode = 0x06,
    MessageStartCalibration = 0x07,
    MessageAbortCalibration = 0x08,
    MessageSetControllerId = 0x09,
    MessageClearFaults = 0x0A,
    MessageIdentify = 0x0B,
    MessageGetCalibration = 0x0C,
    MessageGetAdapterNames = 0x0D,
    MessageSetAdapterNames = 0x0E,

    MessageHelloResponse = 0x81,
    MessageAck = 0x82,
    MessageHeartbeat = 0x83,
    MessageStatus = 0x84,
    MessageConfiguration = 0x85,
    MessageCalibration = 0x86,
    MessageAlert = 0x87,
    MessageAdapterNames = 0x88
};

enum AckStatus : uint8_t {
    AckOk = 0,
    AckInvalidLength = 1,
    AckInvalidValue = 2,
    AckBusy = 3,
    AckStorageFailure = 4,
    AckUnsupported = 5,
    AckUnsafe = 6
};

struct ProtocolFrame {
    static constexpr uint8_t kMaximumPayload = 96;
    uint8_t type;
    uint16_t sequence;
    uint8_t length;
    uint8_t payload[kMaximumPayload];
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, uint8_t length);
    bool getU8(uint8_t& value);
    bool getU16(uint16_t& value);
    bool getI16(int16_t& value);
    bool getU32(uint32_t& value);
    bool getBytes(uint8_t* destination, uint8_t length);
    bool finished() const;

private:
    const uint8_t* data_;
    uint8_t length_;
    uint8_t position_ = 0;
};

class ByteWriter {
public:
    ByteWriter(uint8_t* data, uint8_t capacity);
    bool putU8(uint8_t value);
    bool putU16(uint16_t value);
    bool putI16(int16_t value);
    bool putU32(uint32_t value);
    bool putBytes(const uint8_t* source, uint8_t length);
    uint8_t length() const;
    bool valid() const;

private:
    uint8_t* data_;
    uint8_t capacity_;
    uint8_t length_ = 0;
    bool valid_ = true;
};

class SerialProtocol {
public:
    static constexpr uint8_t kVersion = 2;

    bool poll(Stream& stream, uint32_t nowMs, ProtocolFrame& frame);
    void send(Stream& stream, uint8_t type, uint16_t sequence,
              const uint8_t* payload, uint8_t length);

private:
    enum class ReceiveState : uint8_t {
        Magic1,
        Magic2,
        Version,
        Type,
        SequenceLow,
        SequenceHigh,
        Length,
        Payload,
        CrcLow,
        CrcHigh
    };

    void reset();
    void addToCrc(uint8_t value);

    ReceiveState state_ = ReceiveState::Magic1;
    ProtocolFrame incoming_ = {};
    uint8_t payloadPosition_ = 0;
    uint16_t calculatedCrc_ = 0xFFFF;
    uint16_t receivedCrc_ = 0;
    uint32_t previousByteMs_ = 0;
};

} // namespace fc
