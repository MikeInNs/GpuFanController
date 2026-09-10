#include "ConfigurationStore.h"
#include "Configuration.h"
#include "Crc16.h"
#include <EEPROM.h>
#include <string.h>

namespace fc {

constexpr int ConfigurationStore::kSlotAddresses[2];

bool ConfigurationStore::load(PersistedData& data)
{
    Header headers[2];
    PersistedData slot1Data;
    const bool valid0 = readSlot(0, headers[0], data);
    const bool valid1 = readSlot(1, headers[1], slot1Data);

    if (!valid0 && !valid1) {
        memset(&data, 0, sizeof(data));
        Configuration::setDefaults(data.config);
        Configuration::clearCalibration(data.calibration);
        hasActiveSlot_ = false;
        sequence_ = 0;
        return false;
    }

    if (valid1 && (!valid0 ||
                   static_cast<int32_t>(headers[1].sequence -
                                        headers[0].sequence) > 0)) {
        activeSlot_ = 1;
        data = slot1Data;
        sequence_ = headers[1].sequence;
    } else {
        activeSlot_ = 0;
        sequence_ = headers[0].sequence;
    }

    hasActiveSlot_ = true;
    return true;
}

bool ConfigurationStore::save(const PersistedData& data)
{
    const uint8_t targetSlot = hasActiveSlot_ ? activeSlot_ ^ 1u : 0;
    const int address = kSlotAddresses[targetSlot];
    Header header = {};
    header.schemaVersion = kSchemaVersion;
    header.payloadSize = sizeof(PersistedData);
    header.sequence = sequence_ + 1;
    header.checksum = calculateChecksum(header, data);

    // Invalidate first. The magic is committed last, so interrupted writes leave
    // the previous slot recoverable.
    const uint32_t invalidMagic = 0;
    updateBytes(address, reinterpret_cast<const uint8_t*>(&invalidMagic),
                sizeof(invalidMagic));
    updateBytes(address + sizeof(Header),
                reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    updateBytes(address + offsetof(Header, schemaVersion),
                reinterpret_cast<const uint8_t*>(&header.schemaVersion),
                sizeof(Header) - offsetof(Header, schemaVersion));
    header.magic = kMagic;
    updateBytes(address, reinterpret_cast<const uint8_t*>(&header.magic),
                sizeof(header.magic));

    Header verifyHeader;
    PersistedData verifyData;

    if (!readSlot(targetSlot, verifyHeader, verifyData) ||
        memcmp(&verifyHeader, &header, sizeof(Header)) != 0 ||
        memcmp(&verifyData, &data, sizeof(PersistedData)) != 0) {
        // Do not leave a rejected candidate as the newest committed slot.
        updateBytes(address, reinterpret_cast<const uint8_t*>(&invalidMagic), sizeof(invalidMagic));
        return false;
    }

    activeSlot_ = targetSlot;
    sequence_ = header.sequence;
    hasActiveSlot_ = true;
    return true;
}

bool ConfigurationStore::readSlot(uint8_t slot, Header& header,
                                  PersistedData& data) const
{
    const int address = kSlotAddresses[slot];
    readBytes(address, reinterpret_cast<uint8_t*>(&header), sizeof(header));

    if (header.magic != kMagic || header.schemaVersion != kSchemaVersion ||
        header.payloadSize != sizeof(PersistedData)) {
        return false;
    }

    readBytes(address + sizeof(Header), reinterpret_cast<uint8_t*>(&data),
              sizeof(data));

    return header.checksum == calculateChecksum(header, data) &&
           Configuration::validate(data.config);
}

uint16_t ConfigurationStore::calculateChecksum(
    const Header& header, const PersistedData& data) const
{
    uint16_t checksum = 0xFFFF;
    checksum = crc16(reinterpret_cast<const uint8_t*>(&header.schemaVersion),
                     sizeof(header.schemaVersion), checksum);
    checksum = crc16(reinterpret_cast<const uint8_t*>(&header.payloadSize),
                     sizeof(header.payloadSize), checksum);
    checksum = crc16(reinterpret_cast<const uint8_t*>(&header.sequence),
                     sizeof(header.sequence), checksum);
    return crc16(reinterpret_cast<const uint8_t*>(&data), sizeof(data),
                 checksum);
}

void ConfigurationStore::readBytes(int address, uint8_t* destination,
                                   size_t length) const
{
    for (size_t index = 0; index < length; ++index) {
        destination[index] = EEPROM.read(address + index);
    }
}

void ConfigurationStore::updateBytes(int address, const uint8_t* source,
                                     size_t length)
{
    for (size_t index = 0; index < length; ++index) {
        EEPROM.update(address + index, source[index]);
    }
}

} // namespace fc
