#pragma once

#include <Arduino.h>

namespace fc {

uint16_t crc16Update(uint16_t crc, uint8_t value);
uint16_t crc16(const uint8_t* data, size_t length, uint16_t initial = 0xFFFF);

} // namespace fc
