#include "CalibrationManager.h"
#include "Configuration.h"
#include "ConfigurationStore.h"
#include <EEPROM.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace fc;
void check(bool good){if(!good) throw std::runtime_error("Firmware calibration assertion failed");}
PersistedData original() {
    PersistedData data{};Configuration::setDefaults(data.config);data.controllerId[0]=42;data.config.generation=3;
    data.calibration.generation=7;
    for(auto& g:data.calibration.groups) {
        g.valid=1;g.pointCount=9;g.startDutyPercent[0]=g.startDutyPercent[1]=15;
        for(int i=0;i<9;++i) {g.points[i].dutyPercent=100-i*10;g.points[i].fanRpm[0]=9000-i*700;g.points[i].fanRpm[1]=8900-i*700;g.points[i].voltageMilliVolts=12000;}
    }
    return data;
}
// Hysteretic virtual fans: fan 2 needs 25% to start but only 10% to keep running.
// All timing is simulated; no serial port, GPIO or actual fan is touched.
void completeMeasurements(CalibrationManager& manager,PersistedData& data,const PersistedData& old,
                          uint32_t start=0,bool neverStart=false,bool cannotStop=false) {
    bool spinning[2]={true,true};uint32_t sweepStart=0;
    int previousPhase=-1, directStarts=0, floorChecks=0;
    for(uint32_t elapsed=1000;elapsed<=720000 && manager.active() && manager.status().phase!=CalibrationManager::Complete;elapsed+=1000) {
        const auto s=manager.status();uint16_t rpm[2]={};
        if(s.phase!=previousPhase) {
            directStarts+=s.phase==CalibrationManager::StartVerify;
            floorChecks+=s.phase==CalibrationManager::MinimumVerify;
            previousPhase=s.phase;
        }
        if(s.phase==CalibrationManager::Sweep && !sweepStart) sweepStart=elapsed-1000;
        if(s.phase==CalibrationManager::Sweep && elapsed-sweepStart<10000) check(s.step==0);
        for(int fan=0;fan<2;++fan) {
            if(s.dutyPercent>=(fan?25:15) && !neverStart) spinning[fan]=true;
            if(s.dutyPercent<(fan?10:5)) spinning[fan]=false;
            if(cannotStop) spinning[fan]=true;
            rpm[fan]=spinning[fan]?std::max(500,int(s.dutyPercent)*(fan?90:100)):0;
            if(s.phase==CalibrationManager::Sweep && s.step==0 && spinning[fan]) rpm[fan]+=(elapsed/1000%3)*30;
        }
        manager.update(start+elapsed,rpm[0],rpm[1],{12000,400,true});
        check(std::memcmp(&data,&old,sizeof(data))==0);
    }
    if(manager.status().phase==CalibrationManager::Complete) check(directStarts==2 && floorChecks==2);
}
int main() {
    static_assert(sizeof(GroupCalibration)==87 && sizeof(PersistedData)==266);
    const auto seed=original();auto data=seed;
    CalibrationManager manager;manager.begin(&data.calibration,&data.config);
    check(manager.start(0,data.config.groups[0],0));
    for(uint32_t t=1000;t<10000;t+=1000) {manager.update(t,0,0,{12000,400,true});check(manager.status().phase==CalibrationManager::SpinDown);}
    manager.update(10000,0,0,{12000,400,true});check(manager.status().phase==CalibrationManager::StartSearch);
    manager.abort();check(manager.consumeAborted() && !manager.consumeDataChanged());check(std::memcmp(&data,&seed,sizeof(data))==0);
    check(manager.start(0,data.config.groups[0],0));
    manager.update(8000,90,90,{12000,400,true});manager.update(9000,60,60,{12000,400,true});manager.update(10000,0,0,{12000,400,true});
    check(manager.status().phase==CalibrationManager::SpinDown); // Coasting is not verified stopped.
    for(uint32_t t=11000;t<=13000;t+=1000) manager.update(t,0,0,{12000,400,true});
    check(manager.status().phase==CalibrationManager::StartSearch);manager.abort();

    EEPROM=FakeEEPROM{};ConfigurationStore store;check(store.save(data));
    check(manager.start(0,data.config.groups[0],0));completeMeasurements(manager,data,seed);
    check(manager.status().phase==CalibrationManager::Complete && manager.active());
    check(manager.consumeDataChanged() && !manager.consumeDataChanged());
    check(manager.prepareCommit() && !manager.prepareCommit());
    check(data.calibration.generation==8 && data.config.generation==4);
    const auto& measured=data.calibration.groups[0];
    check(measured.startDutyPercent[0]==15 && measured.startDutyPercent[1]==25);
    check(measured.minimumRunningDutyPercent==13 && measured.reserved==0x83);
    check(measured.points[0].fanRpm[0]==10030 && measured.points[8].dutyPercent==13);
    for(int i=1;i<9;++i) check(measured.points[i].dutyPercent<measured.points[i-1].dutyPercent);
    check(data.config.groups[0].minimumDutyPercent==13 && data.config.groups[0].startupDutyPercent==28 && data.config.groups[0].startupTimeMs==5000);
    check(Configuration::validate(data.config));
    auto unchanged=data;unchanged.config=seed.config;unchanged.calibration.groups[0]=seed.calibration.groups[0];unchanged.calibration.generation=seed.calibration.generation;
    check(std::memcmp(&unchanged,&seed,sizeof(seed))==0);
    const auto candidate=data;
    manager.finishCommit(store.save(data));check(manager.status().phase==CalibrationManager::Saved && !manager.active());
    PersistedData reboot{};ConfigurationStore loaded;check(loaded.load(reboot));check(std::memcmp(&reboot,&candidate,sizeof(reboot))==0);

    data=seed;EEPROM=FakeEEPROM{};ConfigurationStore failing;
    check(failing.save(data));check(failing.save(data));EEPROM.ignoreWrites=true;
    check(manager.start(0,data.config.groups[0],0));completeMeasurements(manager,data,seed);
    check(manager.consumeDataChanged() && manager.prepareCommit());const bool saved=failing.save(data);check(!saved);
    manager.finishCommit(saved);check(manager.status().phase==CalibrationManager::StorageFailed && manager.consumeAborted());
    check(std::memcmp(&data,&seed,sizeof(data))==0);ConfigurationStore reloadOld;check(reloadOld.load(reboot));check(std::memcmp(&reboot,&seed,sizeof(seed))==0);

    EEPROM=FakeEEPROM{};ConfigurationStore initial;check(initial.save(seed));check(initial.save(seed));const auto baseline=EEPROM.bytes;
    for(int cut=0;cut<=284;++cut) {
        EEPROM=FakeEEPROM{};EEPROM.bytes=baseline;ConfigurationStore writing;check(writing.load(reboot));EEPROM.writesLeft=cut;
        try {writing.save(candidate);} catch(const PowerLoss&) {}
        EEPROM.writesLeft=-1;ConfigurationStore boot;check(boot.load(reboot));
        check(std::memcmp(&reboot,&seed,sizeof(seed))==0 || std::memcmp(&reboot,&candidate,sizeof(candidate))==0);
    }
    for(int failure=0;failure<4;++failure) {
        data=seed;check(manager.start(0,data.config.groups[0],0));
        if(failure==0) manager.update(720000,1000,1000,{12000,400,true});
        if(failure==1) manager.update(1000,1000,1000,{0,0,false});
        if(failure==2) for(uint32_t t=1000;t<=18000;t+=1000) manager.update(t,t%2000?7000:12000,9000,{12000,400,true});
        if(failure==3) completeMeasurements(manager,data,seed,0,true);
        const uint8_t phases[]={CalibrationManager::TimedOut,CalibrationManager::PowerUnavailable,CalibrationManager::Unstable,CalibrationManager::InvalidMeasurements};
        check(manager.status().phase==phases[failure] && manager.consumeAborted());
        check(!manager.consumeDataChanged() && std::memcmp(&data,&seed,sizeof(seed))==0);
    }
    data=seed;data.config.generation=data.calibration.generation=UINT32_MAX;const auto wrapOld=data;
    check(manager.start(0,data.config.groups[0],0xffff0000UL));completeMeasurements(manager,data,wrapOld,0xffff0000UL);
    check(manager.prepareCommit());check(data.calibration.generation==0 && data.config.generation==0);
    manager.finishCommit(false);check(std::memcmp(&data,&wrapOld,sizeof(data))==0);
    data=seed;check(manager.start(0,data.config.groups[0],0));completeMeasurements(manager,data,seed,0,false,true);
    check(manager.prepareCommit());check(data.calibration.groups[0].reserved==0x80);
    check(data.config.groups[0].startupDutyPercent==100);manager.finishCommit(false);
    data=seed;data.config.groups[0].expectedFanMask=1;const auto single=data;
    check(manager.start(0,data.config.groups[0],0));completeMeasurements(manager,data,single);
    check(manager.prepareCommit());check(data.calibration.groups[0].minimumRunningDutyPercent==8 && data.calibration.groups[0].reserved==0x81);
    manager.finishCommit(false);check(std::memcmp(&data,&single,sizeof(data))==0);
    std::cout<<"Repeated hysteretic starts, safe running floor, adaptive samples, config/table rollback, 285 power cuts, non-stopping/single fans and wraparound passed\n";
}
