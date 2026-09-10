#include "AmdGpu.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <regex>
#include <vector>

namespace fan {
namespace {
namespace fs=std::filesystem;
bool pciAddress(const std::string& value) {
    static const std::regex pattern("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]");
    return std::regex_match(value,pattern);
}
std::string text(const fs::path& path) {
    std::ifstream stream(path);
    if(!stream) throw std::runtime_error("Cannot read "+path.filename().string());
    char buffer[257];stream.read(buffer,sizeof(buffer));
    if(stream.bad() || stream.gcount()>256) throw std::runtime_error("Invalid sysfs value: "+path.filename().string());
    std::string value(buffer,static_cast<std::size_t>(stream.gcount()));
    const auto first=value.find_first_not_of(" \t\r\n");
    return first==std::string::npos?"":value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
std::string optionalText(const fs::path& path) {
    try {return text(path);} catch(const std::exception&) {return {};}
}
bool gpuClass(const fs::path& path) {
    const auto value=optionalText(path/"class");
    return value.starts_with("0x03") || value.starts_with("0x1200");
}
std::vector<fs::path> entries(const fs::path& path) {
    std::error_code error;
    fs::directory_iterator it(path,error);
    if(error) throw std::runtime_error("Cannot enumerate "+path.string()+": "+error.message());
    std::vector<fs::path> result;
    for(const auto& entry:it) result.push_back(entry.path());
    std::sort(result.begin(),result.end());return result;
}
}
bool AmdGpu::owns(const std::string& pci) const {
    return pciAddress(pci) && optionalText(root_/pci/"vendor")=="0x1002" && gpuClass(root_/pci);
}
bool AmdGpu::hasNvidia() const {
    for(const auto& path:entries(root_))
        if(optionalText(path/"vendor")=="0x10de" && gpuClass(path)) return true;
    return false;
}
fs::path AmdGpu::edgeSensor(const std::string& pci) const {
    if(!owns(pci)) throw std::invalid_argument("Not an AMD GPU PCI device");
    const auto device=root_/pci;
    std::error_code error;
    if(fs::read_symlink(device/"driver",error).filename()!="amdgpu" || error)
        throw std::runtime_error("amdgpu driver not bound");
    std::vector<fs::path> sensors;
    for(const auto& path:entries(device/"hwmon"))
        if(optionalText(path/"name")=="amdgpu") sensors.push_back(path);
    if(sensors.size()!=1) throw std::runtime_error("Expected one amdgpu hwmon device");
    const auto& sensor=sensors.front();
    // Fixed edge channel: never substitute hotspot/memory or a CPU sensor.
    if(fs::exists(sensor/"temp1_label") && text(sensor/"temp1_label")!="edge")
        throw std::runtime_error("Unexpected temp1 sensor label (expected edge)");
    if(fs::exists(sensor/"temp1_fault") && text(sensor/"temp1_fault")!="0")
        throw std::runtime_error("GPU edge sensor reports a fault");
    return sensor;
}
std::int16_t AmdGpu::temperature(const std::string& pci) const {
    const auto sensor=edgeSensor(pci);
    const auto value=text(sensor/"temp1_input");
    int milliC=0;const auto [end,err]=std::from_chars(value.data(),value.data()+value.size(),milliC);
    if(err!=std::errc{} || end!=value.data()+value.size() || milliC< -40000 || milliC>125000)
        throw std::runtime_error("Invalid GPU edge temperature");
    return static_cast<std::int16_t>(std::lround(milliC/100.0));
}
nlohmann::json AmdGpu::thermalLimits(const std::string& pci) const {
    const auto sensor=edgeSensor(pci);
    (void)temperature(pci); // An unavailable/faulted control source cannot seed settings.
    auto limit=[&](const char* file)->nlohmann::json {
        if(!fs::exists(sensor/file)) return nullptr;
        const auto value=text(sensor/file);int milliC=0;
        const auto [end,err]=std::from_chars(value.data(),value.data()+value.size(),milliC);
        if(err!=std::errc{} || end!=value.data()+value.size() || milliC<=0 || milliC>200000)
            throw std::runtime_error(std::string("Invalid AMD edge limit: ")+file);
        return milliC/100; // Round limits down, never up.
    };
    return {{"pciAddress",pci},{"vendor","AMD"},{"sensor","gpu-edge"},
        {"source","amdgpu temp1 edge (experimental)"},{"experimental",true},
        {"limits",{{"targetDeciC",nullptr},{"operatingDeciC",nullptr},{"slowdownDeciC",nullptr},
        {"criticalDeciC",limit("temp1_crit")},{"shutdownDeciC",limit("temp1_emergency")}}}};
}
GpuReadings AmdGpu::sample(const std::set<std::string>& addresses) const {
    GpuReadings result{{},SteadyClock::now(),{}};
    for(const auto& pci:addresses) {
        if(!owns(pci)) continue;
        try {result.temperatures[pci]=temperature(pci);}
        catch(const std::exception& e) {
            if(!result.error.empty()) result.error+="; ";
            result.error+="AMD "+pci+": "+e.what();
        }
    }
    return result;
}
nlohmann::json AmdGpu::discover() const {
    auto result=nlohmann::json::array();
    for(const auto& path:entries(root_)) {
        const auto pci=path.filename().string();
        if(!owns(pci)) continue;
        auto name=optionalText(path/"product_name");
        if(name.empty()) name="AMD GPU (PCI device "+optionalText(path/"device")+")";
        // Avoid passing control characters from device metadata into the terminal.
        for(char& ch:name) if(static_cast<unsigned char>(ch)<32 || static_cast<unsigned char>(ch)>126) ch='?';
        auto reading=sample({pci});
        result.push_back({{"uuid",nullptr},{"pciAddress",pci},{"name",name},{"vendor","AMD"},
            {"temperatureSensor","GPU edge (amdgpu temp1_input)"},
            {"temperatureAvailable",reading.temperatures.contains(pci)},
            {"temperatureError",reading.error},{"experimental",true}});
    }
    return result;
}
}
