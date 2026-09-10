#include "fan/Protocol.hpp"
#include <stdexcept>

namespace fan::protocol {
namespace {
void append16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value));
    data.push_back(static_cast<std::uint8_t>(value >> 8));
}
}
std::uint16_t crc16(std::span<const std::uint8_t> data) {
    std::uint16_t crc = 0xFFFF;
    for (auto byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}
std::vector<std::uint8_t> encode(MessageType type, std::uint16_t sequence,
                                std::span<const std::uint8_t> payload) {
    if (payload.size() > max_payload) throw std::invalid_argument("Payload exceeds 96 bytes");
    std::vector<std::uint8_t> result{0xA5, 0x5A, version, static_cast<std::uint8_t>(type)};
    append16(result, sequence);
    result.push_back(static_cast<std::uint8_t>(payload.size()));
    result.insert(result.end(), payload.begin(), payload.end());
    const auto crc = crc16(std::span(result).subspan(2));
    append16(result, crc);
    return result;
}
std::vector<std::uint8_t> TemperatureSnapshot::payload() const {
    if (valid_mask > 3) throw std::invalid_argument("Unknown temperature validity bits");
    if (((valid_mask & 1) && (group1_deci_celsius < -400 || group1_deci_celsius > 1250)) ||
        ((valid_mask & 2) && (group2_deci_celsius < -400 || group2_deci_celsius > 1250)))
        throw std::invalid_argument("Temperature outside firmware range");
    std::vector<std::uint8_t> result{valid_mask};
    append16(result, static_cast<std::uint16_t>(group1_deci_celsius));
    append16(result, static_cast<std::uint16_t>(group2_deci_celsius));
    return result;
}
} // namespace fan::protocol
