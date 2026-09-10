#include "fan/Protocol.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace fan::protocol;
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Invalid input was accepted");
}
int main() {
    try {
        const std::array<std::uint8_t, 9> standard{'1','2','3','4','5','6','7','8','9'};
        check(crc16(standard) == 0x29B1, "CRC standard check failed");
        const auto frame = encode(MessageType::Temperatures, 0x1234,
                                   TemperatureSnapshot{1,623,0}.payload());
        check(frame == std::vector<std::uint8_t>{0xA5,0x5A,2,2,0x34,0x12,5,1,0x6F,2,0,0,0xF1,0xED},
              "Wire format differs from protocol-v2 golden vector");
        check(TemperatureSnapshot{3,-400,1250}.payload() ==
              std::vector<std::uint8_t>{3,0x70,0xFE,0xE2,4}, "Signed temperature encoding failed");
        rejects([] { (void)TemperatureSnapshot{4,0,0}.payload(); });
        rejects([] { (void)TemperatureSnapshot{1,1251,0}.payload(); });
        rejects([] { (void)encode(MessageType::Hello, 1, std::vector<std::uint8_t>(97)); });
        std::cout << "Protocol vectors and validation passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
