#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace fan::protocol {
inline constexpr std::uint8_t version = 2;
inline constexpr std::size_t max_payload = 96;
enum class MessageType : std::uint8_t {
    Hello = 0x01, Temperatures = 0x02, SetConfiguration = 0x03,
    GetConfiguration = 0x04, GetStatus = 0x05, SetMode = 0x06,
    StartCalibration = 0x07, AbortCalibration = 0x08, SetControllerId = 0x09,
    ClearFaults = 0x0A, Identify = 0x0B, GetCalibration = 0x0C,
    GetAdapterNames = 0x0D, SetAdapterNames = 0x0E,
    HelloResponse = 0x81, Acknowledgment = 0x82, Heartbeat = 0x83,
    Status = 0x84, Configuration = 0x85, Calibration = 0x86, Alert = 0x87,
    AdapterNames = 0x88
};
std::uint16_t crc16(std::span<const std::uint8_t> data);
std::vector<std::uint8_t> encode(MessageType type, std::uint16_t sequence,
                                std::span<const std::uint8_t> payload);
struct TemperatureSnapshot {
    std::uint8_t valid_mask;
    std::int16_t group1_deci_celsius;
    std::int16_t group2_deci_celsius;
    std::vector<std::uint8_t> payload() const;
};
} // namespace fan::protocol
