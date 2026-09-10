#include "SettingsControl.hpp"
#include <stdexcept>
namespace fan::settingsControl {
namespace {
void require(bool b,const char* message) {if(!b) throw std::invalid_argument(message);}
void confirmed(const nlohmann::json& r) {
    require(r.is_object() && r.contains("confirmed") && r["confirmed"].is_boolean() && r["confirmed"]==true,"Explicit confirmation required");
}
}
void validateSupply(const nlohmann::json& r) {
    confirmed(r);
    require(r.size()==3 && r.contains("configGeneration") && r.contains("changes"),"Expected confirmed, configGeneration and changes");
    require(r["configGeneration"].is_number_integer() && r["configGeneration"]>=0 && r["configGeneration"]<=UINT32_MAX,"Invalid configuration generation");
    require(r["changes"].is_object() && !r["changes"].empty(),"Expected nonempty supply changes");
    for(const auto& [key,value]:r["changes"].items()) {
        require(key=="voltageCriticalLowMv" || key=="voltageWarningLowMv" || key=="voltageHighMv","Unsupported supply setting");
        require(value.is_number_integer() && value>=6000 && value<=16000,"Supply thresholds must be integer 6000..16000 mV");
    }
}
std::uint8_t clearMask(const nlohmann::json& r) {
    confirmed(r);
    require(r.size()==2 && r.contains("scope") && r["scope"].is_string(),"Expected confirmed and scope");
    if(r["scope"]=="group1") return 1;
    if(r["scope"]=="group2") return 2;
    if(r["scope"]=="global") return 128;
    if(r["scope"]=="all") return 131;
    throw std::invalid_argument("Scope must be group1, group2, global or all");
}
}
