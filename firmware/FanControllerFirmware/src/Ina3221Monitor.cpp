#include "Ina3221Monitor.h"

namespace fc {

void Ina3221Monitor::begin()
{
    Wire.begin();
    Wire.setClock(100000UL);
    Wire.setWireTimeout(kI2cTimeoutUs, true);
    discover();
}

void Ina3221Monitor::update(uint32_t nowMs)
{
    if (!present_) {
        if (nowMs - previousDiscoveryMs_ >= 5000) {
            discover();
            previousDiscoveryMs_ = nowMs;
        }

        return;
    }

    if (nowMs - previousReadMs_ < 250) {
        return;
    }

    previousReadMs_ = nowMs;
    bool success = true;

    for (uint8_t channelIndex = 0; channelIndex < 3; ++channelIndex) {
        success &= readChannel(channelIndex + 1, channels_[channelIndex]);
    }

    if (!success) {
        present_ = false;

        for (uint8_t channelIndex = 0; channelIndex < 3; ++channelIndex) {
            channels_[channelIndex].valid = false;
        }
    }
}

bool Ina3221Monitor::present() const
{
    return present_;
}

uint8_t Ina3221Monitor::address() const
{
    return address_;
}

bool Ina3221Monitor::sdaHigh() const
{
    return digitalRead(SDA) == HIGH;
}

bool Ina3221Monitor::sclHigh() const
{
    return digitalRead(SCL) == HIGH;
}

const InaChannelReading& Ina3221Monitor::channel(uint8_t oneBasedChannel) const
{
    static const InaChannelReading invalid = {0, 0, false};
    return oneBasedChannel >= 1 && oneBasedChannel <= 3
               ? channels_[oneBasedChannel - 1]
               : invalid;
}

bool Ina3221Monitor::discover()
{
    present_ = false;
    address_ = 0;

    for (uint8_t candidate = 0x40; candidate <= 0x43; ++candidate) {
        Wire.clearWireTimeoutFlag();
        Wire.beginTransmission(candidate);
        const uint8_t result = Wire.endTransmission();
        const bool timedOut = Wire.getWireTimeoutFlag();
        Wire.clearWireTimeoutFlag();

        if (result == 0 && !timedOut && initializeAt(candidate)) {
            address_ = candidate;
            present_ = true;
            previousReadMs_ = millis();
            return true;
        }
    }

    address_ = 0;
    return false;
}

bool Ina3221Monitor::initializeAt(uint8_t address)
{
    address_ = address;
    uint16_t manufacturer;
    uint16_t die;

    if (!readRegister(0xFE, manufacturer) || !readRegister(0xFF, die) ||
        manufacturer != kManufacturerId || die != kDieId) {
        return false;
    }

    return writeRegister(0x00, kConfiguration);
}

bool Ina3221Monitor::readRegister(uint8_t registerAddress, uint16_t& value)
{
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(address_);
    Wire.write(registerAddress);

    if (Wire.endTransmission(false) != 0 || Wire.getWireTimeoutFlag()) {
        Wire.clearWireTimeoutFlag();
        return false;
    }

    const uint8_t received =
        Wire.requestFrom(address_, static_cast<uint8_t>(2));
    const bool timedOut = Wire.getWireTimeoutFlag();
    Wire.clearWireTimeoutFlag();

    if (received != 2 || timedOut) {
        while (Wire.available()) Wire.read();
        return false;
    }

    value = static_cast<uint16_t>(Wire.read()) << 8;
    value |= static_cast<uint8_t>(Wire.read());
    return true;
}

bool Ina3221Monitor::writeRegister(uint8_t registerAddress, uint16_t value)
{
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(address_);
    Wire.write(registerAddress);
    Wire.write(static_cast<uint8_t>(value >> 8));
    Wire.write(static_cast<uint8_t>(value));
    const uint8_t result = Wire.endTransmission();
    const bool timedOut = Wire.getWireTimeoutFlag();
    Wire.clearWireTimeoutFlag();
    return result == 0 && !timedOut;
}

bool Ina3221Monitor::readChannel(uint8_t oneBasedChannel,
                                 InaChannelReading& reading)
{
    const uint8_t shuntRegister = 1 + (oneBasedChannel - 1) * 2;
    const uint8_t busRegister = shuntRegister + 1;
    uint16_t shuntRaw;
    uint16_t busRaw;

    if (!readRegister(shuntRegister, shuntRaw) ||
        !readRegister(busRegister, busRaw)) {
        reading.valid = false;
        return false;
    }

    const int16_t shuntCounts = static_cast<int16_t>(shuntRaw) / 8;
    const uint16_t busCounts = busRaw / 8;

    reading.busMilliVolts = busCounts * 8u;
    // R100 shunt: 40 uV/count / 0.1 ohm = 0.4 mA/count.
    reading.currentMilliAmps =
        static_cast<int16_t>((static_cast<int32_t>(shuntCounts) * 4) / 10);
    reading.valid = true;
    return true;
}

} // namespace fc
