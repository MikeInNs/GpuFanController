#include "GroupControl.hpp"
#include <stdexcept>

namespace fan::groupControl {
namespace {
void require(bool condition,const char* message) {if(!condition) throw std::invalid_argument(message);}
void generation(const nlohmann::json& n) {
    require(n.is_number_integer() && n>=0 && n<=UINT32_MAX,"Invalid configuration/calibration generation");
}
}
void validateMode(const nlohmann::json& r) {
    require(r.is_object() && r.size()==4 && r.contains("confirmed") && r.contains("mode") &&
        r.contains("timeoutSeconds") && r.contains("configGeneration"),"Expected confirmed, mode, timeoutSeconds and configGeneration");
    require(r["confirmed"].is_boolean() && r["confirmed"]==true,"Explicit confirmation required");
    generation(r["configGeneration"]);
    require(r["mode"]=="auto" || r["mode"]=="full" || r["mode"]=="off","Mode must be auto, full or off");
    require(r["timeoutSeconds"].is_number_integer(),"Timeout must be integer seconds");
    if(r["mode"]=="auto") require(r["timeoutSeconds"]==0,"Auto requires timeoutSeconds 0");
    else require(r["timeoutSeconds"]>=1 && r["timeoutSeconds"]<=3600,"Full/Off timeout must be 1..3600 seconds");
}
std::vector<std::uint8_t> modePayload(int group,const nlohmann::json& r) {
    validateMode(r);require(group==0 || group==1,"Invalid fan group");
    const auto timeout=r["timeoutSeconds"].get<unsigned>();
    return {static_cast<std::uint8_t>(group),static_cast<std::uint8_t>(r["mode"]=="auto"?0:r["mode"]=="full"?2:3),
        static_cast<std::uint8_t>(r["mode"]=="full"?100:0),static_cast<std::uint8_t>(timeout),static_cast<std::uint8_t>(timeout>>8)};
}
void validateEdit(const nlohmann::json& r) {
    require(r.is_object() && r.contains("configGeneration") && r.contains("calibrationGeneration") && r.contains("changes") &&
        r.size()==(r.contains("confirmed")?4U:3U),"Invalid group configuration request");
    generation(r["configGeneration"]);generation(r["calibrationGeneration"]);
    require(r["changes"].is_object() && !r["changes"].empty(),"Expected nonempty group changes");
    if(r.contains("confirmed") || r["changes"].contains("enabled") || r["changes"].contains("expectedFanMask") ||
       r["changes"].contains("minimumDutyPercent") || r["changes"].contains("startupDutyPercent") || r["changes"].contains("startupTimeMs"))
        require(r.contains("confirmed") && r["confirmed"].is_boolean() && r["confirmed"]==true,"Explicit confirmation required for fan-group controls");
}
}
