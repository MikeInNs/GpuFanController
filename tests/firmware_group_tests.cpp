#include "FanGroupController.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <source_location>
void check(bool good,const std::source_location where=std::source_location::current()) {
    if(!good) throw std::runtime_error("Firmware group control assertion failed at line "+std::to_string(where.line()));
}
void rampPolicy() {
    fc::PwmRamp ramp;
    ramp.reset(20,0);check(ramp.step(100,0)==20);
    for(uint32_t t=1;t<=1000;++t) check(ramp.step(100,t)==20+t/100);
    check(ramp.step(30,1001)==30);check(ramp.step(30,50000)==30);
    check(ramp.step(100,50001)==30); // Long idle time cannot be spent on a new target.
    check(ramp.step(100,50101)==31);
    check(ramp.step(10,50102)==31);check(ramp.step(10,50435)==31);check(ramp.step(10,50436)==30);
    check(ramp.step(10,51102)==28); // Three points down in a second, including fractional time.
    ramp.reset(50,0xfffffff0UL);check(ramp.step(100,0xfffffff0UL)==50);check(ramp.step(100,84)==51);
    ramp.reset(100,0);ramp.step(20,0);check(ramp.step(20,1000)==97);
    ramp.reset(5,1001);check(ramp.step(80,1001)==5);check(ramp.step(80,1101)==6);
    fc::PwmRamp other;other.reset(80,0);other.step(20,0);check(other.step(20,1000)==77);check(ramp.duty()==6);
}

void rampIntegration() {
    using namespace fc;
    ControllerConfig config{};Configuration::setDefaults(config);
    auto& cfg=config.groups[0];cfg.minimumDutyPercent=20;cfg.startupTimeMs=0;
    cfg.curvePointCount=2;cfg.curve[0]={300,2000};cfg.curve[1]={700,9000};
    GroupCalibration cal{};cal.valid=1;cal.pointCount=9;
    for(int i=0;i<9;++i) {auto& p=cal.points[i];p.dutyPercent=20+10*i;p.fanRpm[0]=p.fanRpm[1]=p.dutyPercent*100;}
    FanGroupController group;group.begin(0,&cfg,&cal,0);group.setTemperature(300,true,0);
    group.synchronizeAppliedDuty(20,0);
    group.update(0,false,2000,2000,{12000,200,true});
    group.setTemperature(700,true,1);group.update(1,false,2000,2000,{12000,200,true});
    check(group.status().dutyPercent==20 && group.status().targetRpm==9000);
    for(uint32_t t=101;t<=7001;t+=100) {
        const auto rpm=group.status().dutyPercent*100;
        group.update(t,false,rpm,rpm,{12000,200,true});
        check(group.status().mode==OperatingMode::Automatic);
        check(group.status().dutyPercent==20+(t-1)/100);
        check(!(group.status().latchedFaults&(GroupFaultFan1Slow|GroupFaultFan2Slow)));
    }
    group.setTemperature(300,true,7002);group.update(7002,false,9000,9000,{12000,200,true});
    group.update(8002,false,9000,9000,{12000,200,true});check(group.status().dutyPercent==87);
    // Calibration bypasses the ramp in both directions, including its sub-minimum probes.
    for(uint8_t duty:{uint8_t(0),uint8_t(5),uint8_t(100),uint8_t(13),uint8_t(90)}) {
        group.setCalibrationOverride(true,duty);group.update(8003,false,0,0,{12000,200,true});
        check(group.status().mode==OperatingMode::Calibration && group.status().dutyPercent==duty);
    }
    group.setCalibrationOverride(false,100);group.synchronizeAppliedDuty(100,8004);
    group.update(8004,false,10000,10000,{12000,200,true});check(group.status().dutyPercent==100);
    group.update(9004,false,10000,10000,{12000,200,true});check(group.status().dutyPercent==97);
    group.setOverride(RequestedMode::Off,0,10,9005);group.update(9005,false,0,0,{12000,200,true});check(group.status().dutyPercent==0);
    group.setOverride(RequestedMode::Full,100,10,9006);group.update(9006,false,0,0,{12000,200,true});check(group.status().dutyPercent==100);
    group.setOverride(RequestedMode::Automatic,0,0,9007);group.synchronizeAppliedDuty(20,9007);
    group.setTemperature(700,true,9007);group.update(9007,false,2000,2000,{12000,200,true});
    for(uint32_t t=9107;t<=13107;t+=100) group.update(t,false,0,0,{12000,200,true});
    check(group.status().activeFaults&GroupFaultFan1Slow);check(group.status().dutyPercent==100); // Never mask a stalled fan while ramping.
    group.setOverride(RequestedMode::Off,0,10,14000);group.update(14000,true,0,0,{12000,200,true});check(group.status().dutyPercent==100);
    group.setTemperature(900,true,14001);group.update(14001,false,0,0,{12000,200,true});check(group.status().dutyPercent==100);
    group.setTemperature(300,false,14002);group.update(14002,false,0,0,{12000,200,true});check(group.status().dutyPercent==100);
    group.setTemperature(300,true,15000);group.setOverride(RequestedMode::Manual,35,10,15000);
    group.update(15000,false,3500,3500,{12000,200,true});check(group.status().dutyPercent==35);
    cfg.enabled=0;group.update(15001,false,3500,3500,{12000,200,true});check(group.status().dutyPercent==0);
    group.update(15002,true,0,0,{12000,200,true});check(group.status().dutyPercent==100);

    // A verified Auto stop never sweeps through the unsafe nonzero region.
    cfg.enabled=1;cfg.minimumDutyPercent=20;cfg.startupDutyPercent=28;cfg.curve[0].targetRpm=0;
    cal.reserved=0x83;cal.minimumRunningDutyPercent=20;cal.startDutyPercent[0]=cal.startDutyPercent[1]=25;
    group.applyConfiguration(&cfg,&cal,16000);group.setOverride(RequestedMode::Automatic,0,0,16000);
    group.synchronizeAppliedDuty(30,16000);group.update(16000,false,3000,3000,{12000,200,true});
    for(uint32_t t=16100;t<=20000;t+=100) {
        group.update(t,false,2000,2000,{12000,200,true});
        check(group.status().dutyPercent==0 || group.status().dutyPercent>=20);
    }
    check(group.status().dutyPercent==0);
    group.setTemperature(700,true,20001);group.update(20001,false,0,0,{12000,200,true});
    check(group.status().mode==OperatingMode::Full && group.status().dutyPercent==28); // Startup is immediate, not a 10%/s ramp.
}
int main() {
    rampPolicy();rampIntegration();
    using namespace fc;
    static_assert(sizeof(ControllerConfig)==72 && sizeof(PersistedData)==266,"EEPROM layout changed");
    PersistedData data{};Configuration::setDefaults(data.config);
    data.calibration.generation=7;data.calibration.groups[0].valid=1;data.calibration.groups[1].valid=1;
    auto candidate=data.config;candidate.groups[1].expectedFanMask=2;
    Configuration::apply(data,candidate);
    check(data.calibration.generation==8 && data.calibration.groups[0].valid && !data.calibration.groups[1].valid);
    candidate.groups[1].enabled=0;Configuration::apply(data,candidate);
    check(data.calibration.generation==8); // Enable changes alone preserve calibration.
    candidate.groups[0].enabled=0;candidate.groups[0].expectedFanMask=0;
    check(Configuration::validate(candidate));candidate.groups[0].enabled=1;check(!Configuration::validate(candidate));
    Configuration::setDefaults(data.config);
    GroupCalibration cal{};
    FanGroupController group;
    group.begin(1,&data.config.groups[1],&cal,100);
    group.setTemperature(500,true,100);
    InaChannelReading power{12000,200,true};
    auto update=[&](uint32_t at,bool alarm=false,uint16_t fan1=10000,uint16_t fan2=10000){group.update(at,alarm,fan1,fan2,power);};
    update(1500);check(group.status().mode==OperatingMode::Automatic);
    group.setOverride(RequestedMode::Off,0,1,1600);update(1601);check(group.status().dutyPercent==0 && group.status().mode==OperatingMode::Off);
    group.setTemperature(900,true,1602);update(1603);check(group.status().mode==OperatingMode::Failsafe && group.status().dutyPercent==100);
    group.setTemperature(500,false,1604);update(1605);check(group.status().dutyPercent==100);
    group.setTemperature(500,true,1700);update(1701);check(group.status().mode==OperatingMode::Off);
    update(3000);check(group.status().mode==OperatingMode::Full); // Expired Off starts boost immediately.
    group.setOverride(RequestedMode::Full,100,300,5000);update(5001);check(group.status().dutyPercent==100);
    group.setOverride(RequestedMode::Off,0,300,5100);update(5101);check(group.status().mode==OperatingMode::Off);
    data.config.groups[1].enabled=0;group.applyConfiguration(&data.config.groups[1],&cal,5200);update(5201);
    check(group.status().mode==OperatingMode::Disabled && group.status().dutyPercent==0);
    update(5202,true);check(group.status().mode==OperatingMode::Alarm && group.status().dutyPercent==100);
    data.config.groups[1].enabled=1;group.applyConfiguration(&data.config.groups[1],&cal,5300);update(5301);
    check(group.status().mode==OperatingMode::Full); // In-place config transition receives startup boost.
    update(7000);check(group.status().mode==OperatingMode::Automatic); // Old Off override was cleared.
    data.config.groups[1].expectedFanMask=2;group.applyConfiguration(&data.config.groups[1],&cal,7100);
    update(9000,false,0,10000);update(13000,false,0,10000);
    check(!(group.status().activeFaults&GroupFaultFan1Slow));
    update(14000,false,10000,0);update(18000,false,10000,0);
    check(group.status().activeFaults&GroupFaultFan2Slow);
    group.setCalibrationAbortedFault();check(group.status().activeFaults&GroupFaultCalibrationAborted);
    update(18500,false,10000,0);check(!(group.status().activeFaults&GroupFaultCalibrationAborted));
    check(group.status().latchedFaults&GroupFaultCalibrationAborted);
    group.clearLatchedFaults();check(group.status().activeFaults&GroupFaultFan2Slow);
    update(19000,false,10000,0);check(group.status().latchedFaults&GroupFaultFan2Slow); // Active fault re-latches; clearing never overrides protection.
    Configuration::setDefaults(data.config);
    data.config.groups[1].minimumDutyPercent=40;data.config.groups[1].startupDutyPercent=70;data.config.groups[1].startupTimeMs=2000;
    check(Configuration::validate(data.config));
    group.applyConfiguration(&data.config.groups[1],&cal,20000);
    group.setOverride(RequestedMode::Automatic,0,0,20000);update(20001);check(group.status().dutyPercent==70);
    update(23000);check(group.status().dutyPercent>=40);
    update(23001,true);check(group.status().dutyPercent==100);
    data.config.groups[1].startupDutyPercent=39;check(!Configuration::validate(data.config));
    data.config.groups[1].startupDutyPercent=70;data.config.inputVoltageCriticalLowMv=12000;check(!Configuration::validate(data.config));
    check(kGroupTachIndexes[1][0]==3 && kGroupTachIndexes[1][1]==2);
    Configuration::setDefaults(data.config);
    auto& cfg=data.config.groups[1];cfg.minimumDutyPercent=1;cfg.startupDutyPercent=1;cfg.startupTimeMs=0;
    cfg.curve[0]={350,0};cfg.curve[1]={550,2000};cfg.curve[2]={700,5000};cfg.curve[3]={820,9000};
    cal={};cal.valid=1;cal.reserved=0x83;cal.pointCount=9;cal.minimumRunningDutyPercent=13;
    cal.startDutyPercent[0]=15;cal.startDutyPercent[1]=25;
    for(int i=0;i<9;++i) {auto& p=cal.points[i];p.dutyPercent=100-87*i/8;p.fanRpm[0]=p.dutyPercent*100;p.fanRpm[1]=p.dutyPercent*90;}
    group.applyConfiguration(&cfg,&cal,30000);group.setOverride(RequestedMode::Automatic,0,0,30000);
    group.synchronizeAppliedDuty(0,30000);
    group.setTemperature(300,true,30000);update(30001,false,0,0);check(group.status().dutyPercent==0 && group.status().targetRpm==0);
    group.setTemperature(360,true,31000);update(31001,false,0,0);
    check(group.status().mode==OperatingMode::Full && group.status().dutyPercent==28 && group.status().targetRpm>=1170);
    update(35000,false,1300,1170);check(group.status().dutyPercent==28);
    update(37000,false,1300,1170);check(group.status().mode==OperatingMode::Automatic && group.status().dutyPercent>=13);
    group.setTemperature(300,true,38000);update(38001,false,0,0);check(group.status().dutyPercent>=13); // Auto stop ramps to the floor first.
    update(38002,true,0,0);check(group.status().dutyPercent==100);
    group.setTemperature(900,true,39000);update(39001);check(group.status().dutyPercent==100);
    group.setTemperature(300,true,40000);cal.reserved=0x80;update(40001);check(group.status().dutyPercent>=13 && group.status().targetRpm>=1170);
    cal.valid=0;update(41000);check(group.status().mode==OperatingMode::Failsafe && group.status().dutyPercent==100);
    std::cout<<"Firmware fan masks, invalidated calibration, startup/re-enable, mode expiry and safety priority passed\n";
}
