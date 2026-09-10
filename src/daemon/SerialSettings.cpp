#include "SerialProbe.hpp"
#include "SettingsControl.hpp"
#include "ControllerData.hpp"
#include "ConfigStore.hpp"
namespace fan {
nlohmann::json SerialProbe::updateSupply(const nlohmann::json& request) {
    using M=protocol::MessageType;
    settingsControl::validateSupply(request);
    // Firmware 1.3 adds the firmware-side calibration/configuration interlock.
    if(!(capabilities_.load()&0x20)) throw ConfigConflict("Upload firmware 1.3.0 or newer before editing supply settings");
    if(status()["calibrationActive"].get<bool>()) throw ConfigConflict("Calibration active; settings are locked");
    const auto bytes=exchange(M::GetConfiguration,M::Configuration);
    if(controller::configuration(bytes)["generation"]!=request["configGeneration"])
        throw ConfigConflict("Nano configuration changed; Read Nano before applying supply thresholds");
    const auto next=controller::editSupply(bytes,request["changes"]);
    const auto ack=exchange(M::SetConfiguration,M::Acknowledgment,next);
    if(ack.size()!=2 || ack[0]!=3 || ack[1]!=0) throw std::runtime_error("Supply write not acknowledged; Read Nano before retrying");
    if(exchange(M::GetConfiguration,M::Configuration)!=next) throw std::runtime_error("Supply readback differs; Read Nano before retrying");
    return snapshot();
}
nlohmann::json SerialProbe::clearLatched(const nlohmann::json& request) {
    using M=protocol::MessageType;
    const auto mask=settingsControl::clearMask(request);
    const auto ack=exchange(M::ClearFaults,M::Acknowledgment,{mask});
    if(ack.size()!=2 || ack[0]!=10 || ack[1]!=0) throw std::runtime_error("Latch clear not acknowledged; read status before retrying");
    // Readback reports reality: active faults may have already re-latched. Never
    // synthesize all-clear events or erase the daemon's alert history.
    return {{"acknowledged",true},{"scope",request["scope"]},{"status",status()}};
}
}
