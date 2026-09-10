#pragma once
#include <Arduino.h>

namespace fc {
struct __attribute__((packed)) AdapterName {
    char pciAddress[13];
    char name[32];
};
struct __attribute__((packed)) AdapterNames {
    uint32_t generation;
    AdapterName groups[2];
};
// Optional display metadata in unused EEPROM space, separate from cooling data.
class AdapterNameStore {
public:
    static bool valid(const AdapterNames& names);
    static void load(const uint8_t* controllerId, AdapterNames& names);
    // 0=success, 2=invalid, 3=stale generation, 4=storage failure.
    static uint8_t save(const uint8_t* controllerId, const AdapterNames& names);
};
static_assert(sizeof(AdapterNames)==94,"Adapter names must fit protocol v2");
}
