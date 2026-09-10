#include "Crc16.h"

namespace fc {

uint16_t crc16Update(uint16_t crc, uint8_t value)
{
    crc ^= static_cast<uint16_t>(value) << 8;

    for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                              : static_cast<uint16_t>(crc << 1);
    }

    return crc;
}

uint16_t crc16(const uint8_t* data, size_t length, uint16_t initial)
{
    uint16_t result = initial;

    for (size_t i = 0; i < length; ++i) {
        result = crc16Update(result, data[i]);
    }

    return result;
}

} // namespace fc
