#include "AdapterNames.h"
#include "ControllerTypes.h"
#include "Crc16.h"
#include <EEPROM.h>
#include <string.h>

namespace fc {
namespace {
const uint32_t magic=0x314E4147UL;
const int addresses[2]={320,832};
struct __attribute__((packed)) Record {
    uint32_t magic;
    uint8_t controllerId[16];
    AdapterNames names;
    uint16_t checksum;
};
static_assert(sizeof(PersistedData)+14<=320,"Cooling EEPROM overlaps adapter metadata");
static_assert(sizeof(Record)<=128,"Adapter EEPROM record exceeds reserved region");
uint16_t checksum(const Record& r) {
    return crc16(reinterpret_cast<const uint8_t*>(&r)+4,sizeof(r)-6,0xFFFF);
}
bool read(uint8_t slot,const uint8_t* id,Record& r) {
    uint8_t* bytes=reinterpret_cast<uint8_t*>(&r);
    for(unsigned i=0;i<sizeof(r);++i) bytes[i]=EEPROM.read(addresses[slot]+i);
    return r.magic==magic && !memcmp(r.controllerId,id,16) && r.checksum==checksum(r) && AdapterNameStore::valid(r.names);
}
int8_t loadRecord(const uint8_t* id,AdapterNames& names) {
    memset(&names,0,sizeof(names));int8_t active=-1;Record record;
    for(uint8_t slot=0;slot<2;++slot) if(read(slot,id,record) &&
        (active<0 || static_cast<int32_t>(record.names.generation-names.generation)>0)) {
        names=record.names;active=slot;
    }
    return active;
}
void invalidate(uint8_t slot) {for(uint8_t i=0;i<4;++i) EEPROM.update(addresses[slot]+i,0);}
}
bool AdapterNameStore::valid(const AdapterNames& names) {
    for(uint8_t g=0;g<2;++g) {
        const AdapterName& label=names.groups[g];
        bool ended=false,nonSpace=false;
        for(uint8_t i=0;i<32;++i) {
            const uint8_t c=label.name[i];
            if(!c) ended=true;
            else if(ended || c<32 || c>126) return false;
            if(c && c!=' ') nonSpace=true;
        }
        if(!ended) return false;
        if(!label.pciAddress[0]) {
            for(uint8_t i=0;i<13;++i) if(label.pciAddress[i]) return false;
            if(label.name[0]) return false;
        } else {
            if(!nonSpace || label.pciAddress[12]) return false;
            for(uint8_t i=0;i<12;++i) {
                const char c=label.pciAddress[i];
                if(i==4 || i==7) {if(c!=':') return false;}
                else if(i==10) {if(c!='.') return false;}
                else if(i==11) {if(c<'0' || c>'7') return false;}
                else if(!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
            }
        }
    }
    return true;
}
void AdapterNameStore::load(const uint8_t* id,AdapterNames& names) {loadRecord(id,names);}
uint8_t AdapterNameStore::save(const uint8_t* id,const AdapterNames& names) {
    if(!valid(names)) return 2;
    AdapterNames current;const int8_t active=loadRecord(id,current);
    if(names.generation!=current.generation) return 3;
    if(!memcmp(names.groups,current.groups,sizeof(names.groups))) return 0;
    const uint8_t target=active<0?0:active^1;
    Record record={};record.magic=magic;memcpy(record.controllerId,id,16);
    record.names=names;++record.names.generation;record.checksum=checksum(record);
    invalidate(target);
    const uint8_t* bytes=reinterpret_cast<const uint8_t*>(&record);
    for(unsigned i=4;i<sizeof(record);++i) EEPROM.update(addresses[target]+i,bytes[i]);
    for(uint8_t i=0;i<4;++i) EEPROM.update(addresses[target]+i,bytes[i]);
    if(!read(target,id,record) || record.names.generation!=names.generation+1 ||
        memcmp(record.names.groups,names.groups,sizeof(names.groups))) {
        invalidate(target);return 4;
    }
    return 0;
}
}
