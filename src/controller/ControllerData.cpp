#include "ControllerData.hpp"
#include <algorithm>
#include <stdexcept>

namespace fan::controller {
namespace {
struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;
    int u8() { return bytes.at(offset++); }
    int u16() { const auto lo=u8(); return lo | (u8()<<8); }
    int i16() { const auto n=u16(); return n>32767?n-65536:n; }
    std::uint32_t u32() { const auto lo=u16(); return lo | (static_cast<std::uint32_t>(u16())<<16); }
};
void require(bool condition, const char* message) {
    if(!condition) throw std::invalid_argument(message);
}
int integer(const Json& value, int low, int high) {
    require(value.is_number_integer(), "Expected an integer");
    const auto n=value.get<std::int64_t>();
    require(n>=low && n<=high, "Setting outside permitted range");
    return static_cast<int>(n);
}
void put16(std::vector<std::uint8_t>& b, std::size_t i, int n) {
    b.at(i)=n&255; b.at(i+1)=(n>>8)&255;
}
}
std::vector<std::uint8_t> configurationBytes(const Json& c) {
    std::vector<std::uint8_t> b;
    auto u8=[&](const Json& v){b.push_back(integer(v,0,255));};
    auto u16=[&](const Json& v){int n=integer(v,0,65535);b.push_back(n&255);b.push_back(n>>8);};
    const auto gen=c.at("generation").get<std::uint32_t>();
    for(int i=0;i<4;++i) b.push_back((gen>>(8*i))&255);
    u16(c.at("hostUpdateTimeoutMs"));u16(c.at("voltageWarningLowMv"));
    u16(c.at("voltageCriticalLowMv"));u16(c.at("voltageHighMv"));
    require(c.at("groups").size()==2,"Expected two groups");
    for(const auto& g:c.at("groups")) {
        require(g.at("enabled").is_boolean(),"Invalid enabled value");
        u8(g.at("enabled").get<bool>()?1:0);u8(g.at("expectedFanMask"));u8(g.at("curve").size());
        u8(g.at("minimumDutyPercent"));u8(g.at("startupDutyPercent"));u16(g.at("startupTimeMs"));
        u16(g.at("warningTemperatureDeciC"));u16(g.at("criticalTemperatureDeciC"));
        u8(g.at("rpmLowThresholdPercent"));u8(g.at("faultDelaySeconds"));u8(g.at("currentDeviationPercent"));
        require(g.at("curve").size()>=1 && g.at("curve").size()<=4,"Invalid curve count");
        for(std::size_t i=0;i<4;++i) {
            u16(i<g.at("curve").size()?g.at("curve")[i].at("temperatureDeciC"):Json(0));
            u16(i<g.at("curve").size()?g.at("curve")[i].at("rpm"):Json(0));
        }
    }
    require(b.size()==72,"Invalid configuration size");return b;
}
Json configuration(const std::vector<std::uint8_t>& bytes) {
    require(bytes.size()==72,"Invalid configuration payload length");
    Reader r{bytes};
    Json result{{"generation",r.u32()},{"hostUpdateTimeoutMs",r.u16()},
        {"voltageWarningLowMv",r.u16()},{"voltageCriticalLowMv",r.u16()},
        {"voltageHighMv",r.u16()},{"groups",Json::array()}};
    for(int index=0;index<2;++index) {
        Json g{{"enabled",r.u8()!=0},{"expectedFanMask",r.u8()}};
        const auto count=r.u8();
        require(count>=1 && count<=4,"Invalid curve point count");
        g["minimumDutyPercent"]=r.u8(); g["startupDutyPercent"]=r.u8();
        g["startupTimeMs"]=r.u16(); g["warningTemperatureDeciC"]=r.i16();
        g["criticalTemperatureDeciC"]=r.i16(); g["rpmLowThresholdPercent"]=r.u8();
        g["faultDelaySeconds"]=r.u8(); g["currentDeviationPercent"]=r.u8();
        g["curve"]=Json::array();
        for(int i=0;i<4;++i) {
            Json point{{"temperatureDeciC",r.i16()},{"rpm",r.u16()}};
            if(i<count) g["curve"].push_back(point);
        }
        result["groups"].push_back(g);
    }
    return result;
}
Json calibration(const std::vector<std::uint8_t>& bytes) {
    require(bytes.size()==91,"Invalid calibration payload length");
    Reader r{bytes};
    const auto group=r.u8(),valid=r.u8(),count=r.u8();
    require(group<2 && (valid<=1 || valid==3 || valid==7 || valid==11 || valid==15) && count<=9,"Invalid calibration header");
    Json result{{"group",group},{"valid",valid!=0},{"startDutyPercent",Json::array({r.u8(),r.u8()})},
        {"minimumRunningDutyPercent",r.u8()},{"generation",r.u32()},{"points",Json::array()}};
    result["measuredMinimum"]=(valid&2)!=0;
    result["stopVerifiedMask"]=(valid>>2)&3;
    for(int i=0;i<9;++i) {
        Json p{{"dutyPercent",r.u8()},{"fanRpm",Json::array({r.u16(),r.u16()})},
               {"currentMa",r.i16()},{"voltageMv",r.u16()}};
        // v2 uses zero volts for unavailable INA samples; never present zero amps as valid then.
        if(p["voltageMv"]==0) { p["voltageMv"]=nullptr; p["currentMa"]=nullptr; }
        if(i<count) result["points"].push_back(p);
    }
    return result;
}
Json status(const std::vector<std::uint8_t>& bytes) {
    require(bytes.size()==58,"Invalid status payload length");
    Reader r{bytes};
    Json result{{"uptimeMs",r.u32()},{"hostUpdateAgeMs",r.u16()},
        {"activeFaults",r.u16()},{"latchedFaults",r.u16()},
        {"inaPresent",r.u8()!=0},{"inaAddress",r.u8()},{"i2cBusLevels",r.u8()},
        {"calibrationActive",r.u8()!=0},{"calibrationGroup",r.u8()},
        {"calibrationPhase",r.u8()},{"calibrationStep",r.u8()},
        {"calibrationDutyPercent",r.u8()},{"groups",Json::array()}};
    for(int i=0;i<2;++i) {
        Json g{{"mode",r.u8()},{"temperatureDeciC",r.i16()},{"temperatureAgeMs",r.u16()},
            {"targetRpm",r.u16()},{"dutyPercent",r.u8()},
            {"fanRpm",Json::array({r.u16(),r.u16()})},{"voltageMv",r.u16()},
            {"currentMa",r.i16()},{"activeFaults",r.u16()},{"latchedFaults",r.u16()}};
        if((g["activeFaults"].get<int>()&1) || (result["activeFaults"].get<int>()&64)) g["temperatureDeciC"]=nullptr;
        if(!result["inaPresent"].get<bool>() || g["voltageMv"]==0) { g["voltageMv"]=nullptr; g["currentMa"]=nullptr; }
        result["groups"].push_back(g);
    }
    return result;
}
Json rpmRange(const Json& cal, const Json& group) {
    if(!cal.value("valid",false) || cal.at("points").size()!=9) return nullptr;
    const bool measured=cal.value("measuredMinimum",false);
    const int mask=group.at("expectedFanMask");
    const int measuredFloor=measured?cal.at("minimumRunningDutyPercent").get<int>():20;
    const int floor=std::max(group.at("minimumDutyPercent").get<int>(),measuredFloor);
    if(mask<1 || mask>3) return nullptr;
    if(measured && (measuredFloor<4 || measuredFloor>92)) return nullptr;
    // Both expected fans must have started. A stopped fan is not an RPM bound.
    for(int fan=0;fan<2;++fan) if((mask&(1<<fan)) &&
        (cal.at("startDutyPercent")[fan]==0 || cal.at("startDutyPercent")[fan]>(measured?100:99))) return nullptr;
    int minimum=30001, maximum=0, usable=0;
    bool full=false;
    bool seen[101]={};
    bool floorSeen=false;
    for(const auto& p:cal.at("points")) {
        const int duty=p.at("dutyPercent");
        if(duty<(measured?measuredFloor:20) || duty>100 || (!measured && duty%10!=0)) return nullptr;
        if(seen[duty]) return nullptr;
        seen[duty]=true;
        floorSeen|=duty==measuredFloor;
        if(duty<floor || duty>100) continue;
        int rpm=65535;
        for(int fan=0;fan<2;++fan) if(mask&(1<<fan)) rpm=std::min(rpm,p.at("fanRpm")[fan].get<int>());
        if(rpm<=0 || rpm>30000) return nullptr;
        minimum=std::min(minimum,rpm); maximum=std::max(maximum,rpm); ++usable;
        full|=duty==100;
    }
    if(!full || usable<2 || minimum>=maximum || (measured && !floorSeen)) return nullptr;
    Json result{{"minimum",minimum},{"maximum",maximum}};
    if(measured) result["canStop"]=(cal.value("stopVerifiedMask",0)&mask)==mask;
    return result;
}
std::vector<std::uint8_t> editConfiguration(std::vector<std::uint8_t> bytes,
    int group, const Json& changes, const Json& cal) {
    require(group>=0 && group<2 && changes.is_object(),"Invalid group changes");
    const auto config=configuration(bytes);
    const auto offset=12+30*group;
    if(changes.contains("minimumDutyPercent") && changes.at("minimumDutyPercent")!=config["groups"][group]["minimumDutyPercent"])
        require(!changes.contains("curve"),"Apply minimum PWM separately, then reload measured RPM bounds before editing the curve");
    if(changes.contains("expectedFanMask") && changes.at("expectedFanMask")!=config["groups"][group]["expectedFanMask"])
        require(!changes.contains("curve"),"Save expected fans, recalibrate, then edit the RPM curve separately");
    for(const auto& [key,value]:changes.items()) {
        if(key=="curve") {
            const auto range=rpmRange(cal,config["groups"][group]);
            require(!range.is_null(),"Complete, usable fan calibration required before editing RPM curves");
            require(value.is_array() && value.size()>=1 && value.size()<=4,"Curve requires 1 to 4 points");
            int previousTemp=-1,previousRpm=0;
            bytes[offset+2]=value.size();
            for(int i=0;i<4;++i) {
                int temp=0,rpm=0;
                if(i<static_cast<int>(value.size())) {
                    temp=integer(value[i].at("temperatureDeciC"),0,1200);
                    rpm=integer(value[i].at("rpm"),0,range["maximum"]);
                    require((rpm==0 && range.value("canStop",false)) || rpm>=range["minimum"].get<int>(),
                        "RPM must be zero (verified fan-stop) or within the measured running range");
                    require(temp>previousTemp && rpm>=previousRpm,"Curve temperatures must increase and RPM must not decrease");
                    previousTemp=temp; previousRpm=rpm;
                }
                put16(bytes,offset+14+i*4,temp); put16(bytes,offset+16+i*4,rpm);
            }
        } else if(key=="enabled") {
            require(value.is_boolean(),"enabled must be boolean");bytes[offset]=value.get<bool>()?1:0;
        } else if(key=="expectedFanMask") bytes[offset+1]=integer(value,0,3);
        else if(key=="minimumDutyPercent") bytes[offset+3]=integer(value,cal.value("measuredMinimum",false)?cal.at("minimumRunningDutyPercent").get<int>():20,100);
        else if(key=="startupDutyPercent") bytes[offset+4]=integer(value,cal.value("measuredMinimum",false)?1:20,100);
        else if(key=="startupTimeMs") put16(bytes,offset+5,integer(value,0,10000));
        else if(key=="warningTemperatureDeciC") put16(bytes,offset+7,integer(value,0,1199));
        else if(key=="criticalTemperatureDeciC") put16(bytes,offset+9,integer(value,1,1200));
        else if(key=="rpmLowThresholdPercent") bytes[offset+11]=integer(value,20,95);
        else if(key=="faultDelaySeconds") bytes[offset+12]=integer(value,1,30);
        else if(key=="currentDeviationPercent") bytes[offset+13]=integer(value,10,100);
        else throw std::invalid_argument("Unsupported setting: "+key);
    }
    const auto next=configuration(bytes);
    require(next["groups"][group]["startupDutyPercent"]>=next["groups"][group]["minimumDutyPercent"],"Startup PWM must be at least minimum PWM");
    if(cal.value("measuredMinimum",false)) {
        int start=cal.at("minimumRunningDutyPercent");
        const int mask=next["groups"][group]["expectedFanMask"];
        for(int fan=0;fan<2;++fan) if(mask&(1<<fan)) start=std::max(start,std::min(100,cal.at("startDutyPercent")[fan].get<int>()+3));
        if(changes.contains("startupDutyPercent")) require(next["groups"][group]["startupDutyPercent"]>=start,"Startup PWM cannot be below the calibrated safe boost");
        if(changes.contains("startupTimeMs")) require(next["groups"][group]["startupTimeMs"]>=5000,"Calibrated startup requires at least 5000 ms");
    }
    require(!next["groups"][group]["enabled"].get<bool>() || next["groups"][group]["expectedFanMask"]!=0,
        "Enabled groups require at least one expected fan");
    require(next["groups"][group]["warningTemperatureDeciC"]<next["groups"][group]["criticalTemperatureDeciC"],"Warning temperature must be below critical temperature");
    const auto generation=config["generation"].get<std::uint32_t>()+1;
    for(int i=0;i<4;++i) bytes[i]=(generation>>(i*8))&255;
    return bytes;
}
std::vector<std::uint8_t> editSupply(std::vector<std::uint8_t> bytes,const Json& changes) {
    const auto config=configuration(bytes);
    require(changes.is_object() && !changes.empty(),"Expected supply threshold changes");
    for(const auto& [key,value]:changes.items()) {
        int offset;
        if(key=="voltageWarningLowMv") offset=6;
        else if(key=="voltageCriticalLowMv") offset=8;
        else if(key=="voltageHighMv") offset=10;
        else throw std::invalid_argument("Unsupported supply setting: "+key);
        put16(bytes,offset,integer(value,6000,16000));
    }
    const auto next=configuration(bytes);
    require(next["voltageCriticalLowMv"]<=next["voltageWarningLowMv"] && next["voltageWarningLowMv"]<next["voltageHighMv"],
        "Supply thresholds require 6000 <= critical-low <= warning-low < high <= 16000 mV");
    const auto generation=config["generation"].get<std::uint32_t>()+std::uint32_t{1};
    for(int i=0;i<4;++i) bytes[i]=(generation>>(i*8))&255;
    return bytes;
}
}
