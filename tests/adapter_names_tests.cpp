#include "AdapterNames.h"
#include "AdapterNames.hpp"
#include <EEPROM.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <source_location>
void check(bool good,const std::source_location where=std::source_location::current()) {
    if(!good) throw std::runtime_error("Adapter-name assertion failed at line "+std::to_string(where.line()));
}
int main() {
    uint8_t id[16]={42};EEPROM.bytes.fill(0xFF);
    fc::AdapterNames data;fc::AdapterNameStore::load(id,data);check(data.generation==0 && !data.groups[0].name[0]);
    strcpy(data.groups[0].pciAddress,"0000:01:00.0");strcpy(data.groups[0].name,"Tesla V100-SXM2-32GB");
    check(fc::AdapterNameStore::valid(data));
    const auto before=EEPROM.bytes;
    check(fc::AdapterNameStore::save(id,data)==0);
    fc::AdapterNames loaded;fc::AdapterNameStore::load(id,loaded);
    check(loaded.generation==1 && !strcmp(loaded.groups[0].name,data.groups[0].name));
    check(fc::AdapterNameStore::save(id,data)==3); // stale edit
    auto image=EEPROM.bytes;
    check(fc::AdapterNameStore::save(id,loaded)==0 && EEPROM.bytes==image); // no wear on no-op
    for(int i=0;i<1024;++i) if(!(i>=320 && i<448) && !(i>=832 && i<960)) check(EEPROM.bytes[i]==before[i]);
    auto changed=loaded;memset(changed.groups[0].name,0,32);strcpy(changed.groups[0].name,"AMD Radeon");
    // Every interrupted byte update must retain the previous committed copy.
    for(int limit=0;limit<120;++limit) {
        EEPROM.bytes=image;EEPROM.writesLeft=limit;
        try {fc::AdapterNameStore::save(id,changed);} catch(const PowerLoss&) {}
        EEPROM.writesLeft=-1;fc::AdapterNameStore::load(id,data);
        check(data.generation==1 && !strcmp(data.groups[0].name,loaded.groups[0].name));
    }
    EEPROM.bytes=image;EEPROM.ignoreWrites=true;
    check(fc::AdapterNameStore::save(id,changed)==4);EEPROM.ignoreWrites=false;
    check(fc::AdapterNameStore::save(id,changed)==0);fc::AdapterNameStore::load(id,data);check(data.generation==2);
    auto raw=std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&data),reinterpret_cast<uint8_t*>(&data)+sizeof(data));
    auto json=fan::adapterNames::decode(raw);check(fan::adapterNames::encode(json)==raw);
    check(json["groups"][0]["name"].get<std::string>()=="AMD Radeon");
    raw[48]='x';bool rejected=false;try{fan::adapterNames::decode(raw);}catch(const std::exception&){rejected=true;}check(rejected);
    changed.groups[0].name[0]='\n';check(!fc::AdapterNameStore::valid(changed));
    id[0]=43;fc::AdapterNameStore::load(id,data);check(data.generation==0 && !data.groups[0].name[0]);
    std::cout<<"Adapter-name codecs, generation, no-op, interrupted writes, storage failure, cooling-region preservation and controller identity passed\n";
}
