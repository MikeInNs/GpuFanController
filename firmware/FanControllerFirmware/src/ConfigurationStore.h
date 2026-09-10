#pragma once

#include <Arduino.h>
#include "ControllerTypes.h"

namespace fc {

class ConfigurationStore {
public:
    bool load(PersistedData& data);
    bool save(const PersistedData& data);

private:
    struct __attribute__((packed)) Header {
        uint32_t magic;
        uint16_t schemaVersion;
        uint16_t payloadSize;
        uint32_t sequence;
        uint16_t checksum;
    };

    static constexpr uint32_t kMagic = 0x46435047UL; // "GPCF"
    static constexpr uint16_t kSchemaVersion = 5;
    static constexpr int kSlotSize = 512;
    static constexpr int kSlotAddresses[2] = {0, kSlotSize};

    bool readSlot(uint8_t slot, Header& header, PersistedData& data) const;
    uint16_t calculateChecksum(const Header& header,
                               const PersistedData& data) const;
    void readBytes(int address, uint8_t* destination, size_t length) const;
    void updateBytes(int address, const uint8_t* source, size_t length);

    uint8_t activeSlot_ = 0;
    uint32_t sequence_ = 0;
    bool hasActiveSlot_ = false;
};

static_assert(sizeof(PersistedData) + 14 <= 512,
              "Persisted firmware data no longer fits an EEPROM slot");

} // namespace fc
