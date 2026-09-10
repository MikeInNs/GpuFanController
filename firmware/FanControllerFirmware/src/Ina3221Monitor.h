#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "ControllerTypes.h"

namespace fc {

class Ina3221Monitor {
public:
    void begin();
    void update(uint32_t nowMs);
    bool present() const;
    uint8_t address() const;
    bool sdaHigh() const;
    bool sclHigh() const;
    const InaChannelReading& channel(uint8_t oneBasedChannel) const;

private:
    static constexpr uint32_t kI2cTimeoutUs = 25000UL;
    static constexpr uint16_t kConfiguration = 0x7527;
    static constexpr uint16_t kManufacturerId = 0x5449;
    static constexpr uint16_t kDieId = 0x3220;

    bool discover();
    bool initializeAt(uint8_t address);
    bool readRegister(uint8_t registerAddress, uint16_t& value);
    bool writeRegister(uint8_t registerAddress, uint16_t value);
    bool readChannel(uint8_t oneBasedChannel, InaChannelReading& reading);

    InaChannelReading channels_[3] = {};
    uint8_t address_ = 0;
    bool present_ = false;
    uint32_t previousReadMs_ = 0;
    uint32_t previousDiscoveryMs_ = 0;
};

} // namespace fc
