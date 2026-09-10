#include "SerialProtocol.h"
#include "Crc16.h"
#include <string.h>

namespace fc {

namespace {
constexpr uint8_t kMagic1 = 0xA5;
constexpr uint8_t kMagic2 = 0x5A;
}

ByteReader::ByteReader(const uint8_t* data, uint8_t length)
    : data_(data), length_(length)
{
}

bool ByteReader::getU8(uint8_t& value)
{
    if (position_ >= length_) return false;
    value = data_[position_++];
    return true;
}

bool ByteReader::getU16(uint16_t& value)
{
    uint8_t low;
    uint8_t high;
    if (!getU8(low) || !getU8(high)) return false;
    value = static_cast<uint16_t>(low) |
            (static_cast<uint16_t>(high) << 8);
    return true;
}

bool ByteReader::getI16(int16_t& value)
{
    uint16_t raw;
    if (!getU16(raw)) return false;
    value = static_cast<int16_t>(raw);
    return true;
}

bool ByteReader::getU32(uint32_t& value)
{
    uint16_t low;
    uint16_t high;
    if (!getU16(low) || !getU16(high)) return false;
    value = static_cast<uint32_t>(low) |
            (static_cast<uint32_t>(high) << 16);
    return true;
}

bool ByteReader::getBytes(uint8_t* destination, uint8_t length)
{
    if (static_cast<uint16_t>(position_) + length > length_) return false;
    memcpy(destination, data_ + position_, length);
    position_ += length;
    return true;
}

bool ByteReader::finished() const
{
    return position_ == length_;
}

ByteWriter::ByteWriter(uint8_t* data, uint8_t capacity)
    : data_(data), capacity_(capacity)
{
}

bool ByteWriter::putU8(uint8_t value)
{
    if (length_ >= capacity_) {
        valid_ = false;
        return false;
    }
    data_[length_++] = value;
    return true;
}

bool ByteWriter::putU16(uint16_t value)
{
    return putU8(static_cast<uint8_t>(value)) &&
           putU8(static_cast<uint8_t>(value >> 8));
}

bool ByteWriter::putI16(int16_t value)
{
    return putU16(static_cast<uint16_t>(value));
}

bool ByteWriter::putU32(uint32_t value)
{
    return putU16(static_cast<uint16_t>(value)) &&
           putU16(static_cast<uint16_t>(value >> 16));
}

bool ByteWriter::putBytes(const uint8_t* source, uint8_t length)
{
    if (static_cast<uint16_t>(length_) + length > capacity_) {
        valid_ = false;
        return false;
    }
    memcpy(data_ + length_, source, length);
    length_ += length;
    return true;
}

uint8_t ByteWriter::length() const { return length_; }
bool ByteWriter::valid() const { return valid_; }

bool SerialProtocol::poll(Stream& stream, uint32_t nowMs, ProtocolFrame& frame)
{
    if (state_ != ReceiveState::Magic1 && nowMs - previousByteMs_ > 250) {
        reset();
    }

    while (stream.available() > 0) {
        const uint8_t value = static_cast<uint8_t>(stream.read());
        previousByteMs_ = nowMs;

        switch (state_) {
            case ReceiveState::Magic1:
                if (value == kMagic1) state_ = ReceiveState::Magic2;
                break;

            case ReceiveState::Magic2:
                if (value == kMagic2) {
                    state_ = ReceiveState::Version;
                    calculatedCrc_ = 0xFFFF;
                    payloadPosition_ = 0;
                } else {
                    state_ = value == kMagic1 ? ReceiveState::Magic2
                                              : ReceiveState::Magic1;
                }
                break;

            case ReceiveState::Version:
                if (value != kVersion) {
                    reset();
                    break;
                }
                addToCrc(value);
                state_ = ReceiveState::Type;
                break;

            case ReceiveState::Type:
                incoming_.type = value;
                addToCrc(value);
                state_ = ReceiveState::SequenceLow;
                break;

            case ReceiveState::SequenceLow:
                incoming_.sequence = value;
                addToCrc(value);
                state_ = ReceiveState::SequenceHigh;
                break;

            case ReceiveState::SequenceHigh:
                incoming_.sequence |= static_cast<uint16_t>(value) << 8;
                addToCrc(value);
                state_ = ReceiveState::Length;
                break;

            case ReceiveState::Length:
                if (value > ProtocolFrame::kMaximumPayload) {
                    reset();
                    break;
                }
                incoming_.length = value;
                addToCrc(value);
                state_ = value == 0 ? ReceiveState::CrcLow
                                    : ReceiveState::Payload;
                break;

            case ReceiveState::Payload:
                incoming_.payload[payloadPosition_++] = value;
                addToCrc(value);
                if (payloadPosition_ == incoming_.length)
                    state_ = ReceiveState::CrcLow;
                break;

            case ReceiveState::CrcLow:
                receivedCrc_ = value;
                state_ = ReceiveState::CrcHigh;
                break;

            case ReceiveState::CrcHigh:
                receivedCrc_ |= static_cast<uint16_t>(value) << 8;
                if (receivedCrc_ == calculatedCrc_) {
                    frame = incoming_;
                    reset();
                    return true;
                }
                reset();
                break;
        }
    }

    return false;
}

void SerialProtocol::send(Stream& stream, uint8_t type, uint16_t sequence,
                          const uint8_t* payload, uint8_t length)
{
    if (length > ProtocolFrame::kMaximumPayload) return;

    uint16_t checksum = 0xFFFF;
    stream.write(kMagic1);
    stream.write(kMagic2);

    const uint8_t header[] = {
        kVersion,
        type,
        static_cast<uint8_t>(sequence),
        static_cast<uint8_t>(sequence >> 8),
        length
    };

    for (uint8_t index = 0; index < sizeof(header); ++index) {
        stream.write(header[index]);
        checksum = crc16Update(checksum, header[index]);
    }

    for (uint8_t index = 0; index < length; ++index) {
        stream.write(payload[index]);
        checksum = crc16Update(checksum, payload[index]);
    }

    stream.write(static_cast<uint8_t>(checksum));
    stream.write(static_cast<uint8_t>(checksum >> 8));
}

void SerialProtocol::reset()
{
    state_ = ReceiveState::Magic1;
    payloadPosition_ = 0;
    calculatedCrc_ = 0xFFFF;
    receivedCrc_ = 0;
}

void SerialProtocol::addToCrc(uint8_t value)
{
    calculatedCrc_ = crc16Update(calculatedCrc_, value);
}

} // namespace fc
