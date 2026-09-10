#pragma once
#include <array>
#include <cstdint>
struct PowerLoss {};
struct FakeEEPROM {
    std::array<uint8_t,1024> bytes{};
    int writesLeft=-1;
    bool ignoreWrites=false;
    uint8_t read(int address)const{return bytes.at(address);}
    void update(int address,uint8_t value) {
        if(writesLeft==0) throw PowerLoss{};
        if(writesLeft>0) --writesLeft;
        if(!ignoreWrites) bytes.at(address)=value;
    }
};
inline FakeEEPROM EEPROM;
