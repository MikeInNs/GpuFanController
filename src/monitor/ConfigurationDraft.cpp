#include "ConfigurationDraft.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace fan {
namespace {
using Json = nlohmann::json;
constexpr std::array<std::string_view,11> groupKeys{
    "enabled", "expectedFanMask", "minimumDutyPercent", "startupDutyPercent",
    "startupTimeMs", "warningTemperatureDeciC", "criticalTemperatureDeciC",
    "rpmLowThresholdPercent", "faultDelaySeconds", "currentDeviationPercent", "curve"};
constexpr std::array<std::string_view,3> supplyKeys{
    "voltageCriticalLowMv", "voltageWarningLowMv", "voltageHighMv"};
void require(bool condition, const char* message) {
    if(!condition) throw std::invalid_argument(message);
}
std::uint32_t counter(const Json& value) {
    require(value.is_number_integer() && value >= 0 &&
            value <= std::numeric_limits<std::uint32_t>::max(), "Invalid configuration generation");
    return value.get<std::uint32_t>();
}
Json supplyValues(const Json& config) {
    Json result = Json::object();
    for(auto key:supplyKeys) result[std::string(key)] = config.at(std::string(key));
    return result;
}
Json difference(const Json& before, const Json& after) {
    Json result = Json::object();
    for(const auto& [key,value]:after.items())
        if(!before.contains(key) || before.at(key) != value) result[key] = value;
    return result;
}
void checkHost(const Json& config) {
    require(config.is_object() && config.at("controllers").is_array(), "Read host configuration first");
    counter(config.at("revision"));
    require(config.at("schemaVersion").is_number_integer() && config.at("schemaVersion") == 1,
            "Unsupported host configuration schema");
}
void checkSnapshot(const std::string& id, const Json& snapshot) {
    require(id.size() == 32 && id != std::string(32,'0') &&
        std::all_of(id.begin(),id.end(),[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');}),
        "A registered controller identity is required");
    require(snapshot.at("controllerId") == id, "Nano snapshot identity mismatch");
    const auto& config = snapshot.at("configuration");
    counter(config.at("generation"));
    require(config.at("groups").is_array() && config.at("groups").size() == 2 &&
        snapshot.at("calibration").is_array() && snapshot.at("calibration").size() == 2,
        "Nano snapshot must contain both groups and calibration tables");
    for(int g=0;g<2;++g) {
        require(config["groups"][g].is_object(), "Invalid Nano group");
        for(auto key:groupKeys) config["groups"][g].at(std::string(key));
        counter(snapshot["calibration"][g].at("generation"));
    }
    supplyValues(config);
    require(snapshot.at("status").at("calibrationActive").is_boolean(), "Missing calibration state");
    if(config.contains("adapterNames")) {
        const auto& names = config.at("adapterNames");
        counter(names.at("generation"));
        require(names.at("groups").is_array() && names.at("groups").size() == 2,
                "Invalid adapter-name groups");
    }
}
}

ConfigurationDraft::Nano ConfigurationDraft::fromSnapshot(const Json& snapshot) {
    const auto& c = snapshot.at("configuration");
    return {snapshot,snapshot,{c.at("groups")[0],c.at("groups")[1]},
            supplyValues(c),c.value("adapterNames",Json(nullptr))};
}
void ConfigurationDraft::observeHost(const Json& saved) {
    checkHost(saved);
    if(!hostDirty()) hostBaseline_ = hostDesired_ = saved;
    hostLatest_ = saved;
}
void ConfigurationDraft::editHost(const Json& desired) {
    require(!hostBaseline_.is_null(), "Read host configuration first");
    checkHost(desired);
    require(desired.at("revision") == hostBaseline_.at("revision"), "Host revision is read-only");
    hostDesired_ = desired;
}
void ConfigurationDraft::observeNano(const std::string& id, const Json& snapshot) {
    checkSnapshot(id,snapshot); // Failed/foreign reads leave existing drafts untouched.
    if(!nanos_.contains(id) || !nanoDirty(id)) nanos_.insert_or_assign(id,fromSnapshot(snapshot));
    else nanos_.at(id).latest = snapshot;
}
const ConfigurationDraft::Json& ConfigurationDraft::baselineNano(const std::string& id) const {
    return nanos_.at(id).baseline;
}
const ConfigurationDraft::Json& ConfigurationDraft::latestNano(const std::string& id) const {
    return nanos_.at(id).latest;
}
void ConfigurationDraft::checkGroup(int index) { require(index==0 || index==1,"Invalid fan group"); }
const ConfigurationDraft::Json& ConfigurationDraft::group(const std::string& id,int index) const {
    checkGroup(index);return nanos_.at(id).groups[index];
}
const ConfigurationDraft::Json& ConfigurationDraft::supply(const std::string& id) const {
    return nanos_.at(id).supply;
}
const ConfigurationDraft::Json& ConfigurationDraft::adapterNames(const std::string& id) const {
    return nanos_.at(id).names;
}
void ConfigurationDraft::editGroup(const std::string& id,int index,const std::string& key,const Json& value) {
    checkGroup(index);
    require(std::find(groupKeys.begin(),groupKeys.end(),key)!=groupKeys.end(), "Not an editable group setting");
    nanos_.at(id).groups[index][key] = value;
}
void ConfigurationDraft::editSupply(const std::string& id,const std::string& key,const Json& value) {
    require(std::find(supplyKeys.begin(),supplyKeys.end(),key)!=supplyKeys.end(), "Not an editable supply setting");
    nanos_.at(id).supply[key] = value;
}
void ConfigurationDraft::editAdapterNames(const std::string& id,const Json& groups) {
    auto& names = nanos_.at(id).names;
    require(!names.is_null(), "Firmware does not support adapter-name storage");
    require(groups.is_array() && groups.size()==2,"Adapter labels require both groups");
    for(const auto& group:groups)
        require(group.is_object() && group.size()==2 && group.contains("pciAddress") && group.contains("name"),
                "Adapter labels contain only PCI address and name");
    names["groups"] = groups; // Generation is always the read-only baseline value.
}
bool ConfigurationDraft::hostDirty() const { return hostDesired_ != hostBaseline_; }
bool ConfigurationDraft::nanoDirty(const std::string& id) const {
    const auto& n = nanos_.at(id);const auto& c = n.baseline.at("configuration");
    return n.groups[0]!=c.at("groups")[0] || n.groups[1]!=c.at("groups")[1] ||
        n.supply!=supplyValues(c) || n.names!=c.value("adapterNames",Json(nullptr));
}
std::size_t ConfigurationDraft::dirtyControllers() const {
    return std::count_if(nanos_.begin(),nanos_.end(),[this](const auto& item){return nanoDirty(item.first);});
}
bool ConfigurationDraft::dirty() const { return hostDirty() || dirtyControllers()!=0; }
std::vector<DraftChange> ConfigurationDraft::changes() const {
    std::vector<DraftChange> result;
    if(hostDirty()) result.push_back({{},DraftScope::Host,hostBaseline_,hostDesired_,
        Json::diff(hostBaseline_,hostDesired_),counter(hostBaseline_.at("revision"))});
    for(const auto& [id,n]:nanos_) {
        const auto& c = n.baseline.at("configuration");
        for(int g=0;g<2;++g) {
            auto patch = difference(c.at("groups")[g],n.groups[g]);
            if(!patch.empty()) result.push_back({id,g==0?DraftScope::Group1:DraftScope::Group2,
                c.at("groups")[g],n.groups[g],patch,counter(c.at("generation")),
                n.baseline.at("calibration")[g].at("generation")});
        }
        const auto supply = supplyValues(c);auto patch = difference(supply,n.supply);
        if(!patch.empty()) result.push_back({id,DraftScope::Supply,supply,n.supply,patch,counter(c.at("generation"))});
        if(n.names!=c.value("adapterNames",Json(nullptr)))
            result.push_back({id,DraftScope::AdapterNames,c.at("adapterNames"),n.names,
                {{"groups",n.names.at("groups")}},counter(c.at("adapterNames").at("generation"))});
    }
    return result;
}
std::vector<DraftConflict> ConfigurationDraft::conflicts() const {
    std::vector<DraftConflict> result;
    for(const auto& change:changes()) {
        if(change.scope==DraftScope::Host) {
            if(hostLatest_!=hostBaseline_) result.push_back({{},change.scope,"Host configuration changed since editing began"});
            continue;
        }
        const auto& n = nanos_.at(change.controllerId);
        const auto& old = n.baseline.at("configuration");const auto& now = n.latest.at("configuration");
        bool stale = false;
        if(change.scope==DraftScope::AdapterNames) stale = old.at("adapterNames")!=now.value("adapterNames",Json(nullptr));
        else {
            // Names have a separate counter and do not invalidate cooling edits.
            auto before = old, after = now;before.erase("adapterNames");after.erase("adapterNames");
            stale = before!=after;
            if(change.scope==DraftScope::Group1 || change.scope==DraftScope::Group2) {
                const int g = change.scope==DraftScope::Group1?0:1;
                stale |= n.baseline.at("calibration")[g]!=n.latest.at("calibration")[g];
            }
        }
        if(n.latest.at("status").at("calibrationActive")==true)
            result.push_back({change.controllerId,change.scope,"Calibration is active; finish or abort it before saving"});
        else if(stale) result.push_back({change.controllerId,change.scope,"Nano settings or calibration changed since editing began"});
    }
    return result;
}
void ConfigurationDraft::rebaseHost(const Json& fresh) {
    checkHost(fresh);auto desired=hostDesired_;desired["revision"]=fresh.at("revision");
    hostBaseline_=hostLatest_=fresh;hostDesired_=desired;
}
void ConfigurationDraft::acceptHost(const Json& verified) {
    checkHost(verified);hostBaseline_=hostLatest_=hostDesired_=verified;
}
void ConfigurationDraft::acceptNano(const std::string& id,const Json& verified,DraftScope scope) {
    checkSnapshot(id,verified);auto pending=changes();
    nanos_.at(id)=fromSnapshot(verified);
    for(const auto& c:pending) if(c.controllerId==id && c.scope!=scope) {
        if(c.scope==DraftScope::AdapterNames) editAdapterNames(id,c.after.at("groups"));
        else for(const auto& [key,value]:c.patch.items()) {
            if(c.scope==DraftScope::Supply) editSupply(id,key,value);
            else editGroup(id,c.scope==DraftScope::Group1?0:1,key,value);
        }
    }
}
void ConfigurationDraft::rebaseNano(const std::string& id,const Json& fresh) {
    // Explicit reconciliation only; ordinary observation never calls this.
    acceptNano(id,fresh,DraftScope::Host);
}
void ConfigurationDraft::discardHost() { hostBaseline_ = hostDesired_ = hostLatest_; }
void ConfigurationDraft::discardNano(const std::string& id) {
    auto& n = nanos_.at(id);n = fromSnapshot(n.latest);
}
void ConfigurationDraft::discardAll() {
    discardHost();
    for(auto& [id,n]:nanos_) { (void)id;n = fromSnapshot(n.latest); }
}
}
