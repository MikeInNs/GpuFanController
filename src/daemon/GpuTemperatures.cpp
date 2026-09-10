#include "GpuTemperatures.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <regex>
#include <sstream>

namespace fan {
GpuReadings NvidiaTemperatures::parse(const std::string& csv,SteadyClock::time_point sampledAt) {
    GpuReadings result{{},sampledAt,{}};
    std::istringstream lines(csv);std::string line;
    std::map<std::string,bool> seen;
    auto trim=[](std::string s) {auto first=s.find_first_not_of(" \r\n\t");return first==std::string::npos?std::string{}:s.substr(first,s.find_last_not_of(" \r\n\t")-first+1);};
    while(std::getline(lines,line)) {
        if(trim(line).empty()) continue;
        const auto comma=line.find(',');
        if(comma==std::string::npos || line.find(',',comma+1)!=std::string::npos) throw std::runtime_error("Malformed GPU temperature output");
        auto pci=trim(line.substr(0,comma)),value=trim(line.substr(comma+1));
        std::transform(pci.begin(),pci.end(),pci.begin(),[](unsigned char c){return std::tolower(c);});
        if(pci.size()==16 && pci.substr(0,4)=="0000") pci.erase(0,4);
        if(!std::regex_match(pci,std::regex("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]"))) throw std::runtime_error("Invalid GPU PCI identity");
        if(seen.contains(pci)) throw std::runtime_error("Duplicate GPU PCI identity");
        seen[pci]=true;
        double celsius=0;
        auto [end,err]=std::from_chars(value.data(),value.data()+value.size(),celsius);
        if(err!=std::errc{} || end!=value.data()+value.size() || !std::isfinite(celsius) || celsius< -40 || celsius>125) {
            if(result.error.size()<768) {
                if(!result.error.empty()) result.error+="; ";
                result.error+="GPU "+pci+" temperature unavailable: "+value.substr(0,128);
            }
            continue;
        }
        result.temperatures[pci]=static_cast<std::int16_t>(std::lround(celsius*10));
    }
    if(result.temperatures.empty() && result.error.empty()) result.error="No NVIDIA GPU temperatures available";
    return result;
}
protocol::TemperatureSnapshot mappedTemperatures(const nlohmann::json& groups,const GpuReadings& readings,SteadyClock::time_point now) {
    protocol::TemperatureSnapshot snapshot{0,0,0};
    for(int g=0;g<2;++g) {
        const auto& mapping=groups.at(g);
        if(!mapping.at("enabled").get<bool>() || mapping.at("gpuPciAddress").is_null()) continue;
        auto found=readings.temperatures.find(mapping.at("gpuPciAddress").get<std::string>());
        if(found==readings.temperatures.end()) continue;
        const auto sampledAt=readings.timestampFor(found->first);
        if(sampledAt==SteadyClock::time_point{} || now<sampledAt || now-sampledAt>=std::chrono::seconds(3)) continue;
        snapshot.valid_mask|=1u<<g;
        (g==0?snapshot.group1_deci_celsius:snapshot.group2_deci_celsius)=found->second;
    }
    return snapshot;
}
protocol::TemperatureSnapshot mappedTemperatures(const nlohmann::json& groups,std::span<const GpuReadings> sources,SteadyClock::time_point now) {
    protocol::TemperatureSnapshot result{0,0,0};
    unsigned seen=0,duplicates=0;
    for(const auto& source:sources) {
        const auto sample=mappedTemperatures(groups,source,now);
        duplicates|=seen & sample.valid_mask;seen|=sample.valid_mask;
        if(sample.valid_mask&1) result.group1_deci_celsius=sample.group1_deci_celsius;
        if(sample.valid_mask&2) result.group2_deci_celsius=sample.group2_deci_celsius;
    }
    result.valid_mask=seen & ~duplicates;
    if(!(result.valid_mask&1)) result.group1_deci_celsius=0;
    if(!(result.valid_mask&2)) result.group2_deci_celsius=0;
    return result;
}
bool TemperatureSchedule::due(const protocol::TemperatureSnapshot& snapshot,SteadyClock::time_point now) const {
    return now>=nextDue(snapshot,now);
}
SteadyClock::time_point TemperatureSchedule::nextDue(const protocol::TemperatureSnapshot& snapshot,SteadyClock::time_point now) const {
    if(!previous_) return now;
    return sentAt_+(snapshot.payload()!=*previous_?std::chrono::seconds(1):std::chrono::seconds(5));
}
void TemperatureSchedule::sent(const protocol::TemperatureSnapshot& snapshot,SteadyClock::time_point now) {previous_=snapshot.payload();sentAt_=now;}
}
