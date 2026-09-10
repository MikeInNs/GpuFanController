#include "ConfigurationDraft.hpp"
#include <iostream>
#include <stdexcept>

using Json = nlohmann::json;
using fan::ConfigurationDraft;
using fan::DraftScope;
namespace {
const std::string a(32,'a'), b(32,'b');
int checks = 0;
void require(bool ok,const char* message) {
    ++checks;if(!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F f) {
    bool threw = false;try { f(); } catch(const std::exception&) { threw = true; }
    require(threw,"Expected rejection");
}
Json host() {
    return {{"schemaVersion",1},{"revision",3},{"controllers",Json::array({
        {{"controllerId",a},{"name","Main"},{"groups",Json::array({
            {{"index",0},{"enabled",true},{"gpuPciAddress","0000:01:00.0"}},
            {{"index",1},{"enabled",false},{"gpuPciAddress",nullptr}}})}}})}};
}
Json snapshot(const std::string& id,bool names=true) {
    Json group{{"enabled",true},{"expectedFanMask",3},{"minimumDutyPercent",20},
        {"startupDutyPercent",100},{"startupTimeMs",1000},{"warningTemperatureDeciC",750},
        {"criticalTemperatureDeciC",850},{"rpmLowThresholdPercent",60},
        {"faultDelaySeconds",3},{"currentDeviationPercent",40},
        {"curve",Json::array({{{"temperatureDeciC",300},{"rpm",2500}},
                             {{"temperatureDeciC",850},{"rpm",12000}}})}};
    Json cal{{"generation",7},{"valid",true},{"points",Json::array()}};
    Json config{{"generation",4},{"hostUpdateTimeoutMs",10000},{"groups",Json::array({group,group})},
        {"voltageCriticalLowMv",10500},{"voltageWarningLowMv",11000},{"voltageHighMv",13200}};
    if(names) config["adapterNames"] = {{"generation",2},{"groups",Json::array({
        {{"pciAddress","0000:01:00.0"},{"name","Tesla V100"}},{{"pciAddress",nullptr},{"name",""}}})}};
    return {{"controllerId",id},{"configuration",config},{"calibration",Json::array({cal,cal})},
            {"status",{{"calibrationActive",false},{"uptimeMs",1000}}}};
}
ConfigurationDraft loaded() {
    ConfigurationDraft d;d.observeHost(host());d.observeNano(a,snapshot(a));d.observeNano(b,snapshot(b));return d;
}
}
int main() try {
    auto d = loaded();
    require(!d.dirty() && d.changes().empty(),"Initial reads must not create edits");
    auto h = d.host();h["notifications"]={{"criticalBeep",true}};d.editHost(h);
    d.editGroup(a,0,"warningTemperatureDeciC",700);
    d.editGroup(a,1,"faultDelaySeconds",5);
    d.editSupply(a,"voltageHighMv",13000);
    auto names = d.adapterNames(a).at("groups");names[0]["name"]="Tesla V100 32GB";
    d.editAdapterNames(a,names);d.editGroup(b,0,"startupTimeMs",2000);
    require(d.dirtyControllers()==2 && d.hostDirty(),"One workspace must retain all destinations");
    auto edits = d.changes();require(edits.size()==6,"Six independent persistence scopes");
    require(edits[0].scope==DraftScope::Host && edits[0].generation==3,"Host revision captured");
    require(edits[1].scope==DraftScope::Group1 && edits[1].generation==4 &&
        edits[1].calibrationGeneration==7,"Config and calibration generations captured");
    require(edits[1].patch==Json{{"warningTemperatureDeciC",700}},"Only changed fields are included");
    require(edits[4].scope==DraftScope::AdapterNames && edits[4].generation==2,
            "Names have their own generation");
    require(d.group(a,1).at("faultDelaySeconds")==5 && d.group(b,0).at("startupTimeMs")==2000,
            "Selection-independent group edits");
    edits[1].patch["warningTemperatureDeciC"] = 1;
    require(d.group(a,0).at("warningTemperatureDeciC")==700,"Review copies cannot mutate draft");
    require(d.baselineNano(a)==snapshot(a) && d.latestNano(a)==snapshot(a),"Edits do not alter observations");

    // Read/refresh must never turn a matching RAM observation into 'persisted'.
    auto observed = snapshot(a);observed["status"]["uptimeMs"]=2000;d.observeNano(a,observed);
    require(d.conflicts().empty() && d.nanoDirty(a),"Live telemetry must not dirty/conflict settings");
    observed["configuration"]["groups"][0]["warningTemperatureDeciC"]=700;
    observed["configuration"]["generation"]=5;d.observeNano(a,observed);
    require(d.nanoDirty(a) && d.changes().size()==6,"Readback alone must not clear pending changes");
    require(d.conflicts().size()==3,"Shared cooling generation conflicts with both groups and supply");
    require(d.group(a,1).at("faultDelaySeconds")==5,"Refresh preserves the other group's edits");
    d.discardNano(a);
    require(!d.nanoDirty(a) && d.nanoDirty(b) && d.hostDirty(),"Scoped discard preserves unrelated edits");
    require(d.group(a,0).at("warningTemperatureDeciC")==700,"Discard adopts latest read, not original hardware state");

    auto externalHost = host();externalHost["revision"]=4;externalHost["controllers"][0]["name"]="External";
    d.observeHost(externalHost);
    require(d.host().at("notifications").at("criticalBeep")==true && d.conflicts().size()==1,
            "Host reload preserves notification edit and exposes conflict");
    d.discardHost();require(d.host()==externalHost && !d.hostDirty(),"Explicit host discard reloads latest");
    d.discardAll();require(!d.dirty() && d.changes().empty(),"Explicit global discard");

    d = loaded();d.editGroup(a,0,"faultDelaySeconds",5);d.editGroup(a,0,"faultDelaySeconds",3);
    require(!d.dirty(),"Reverting to baseline removes no-op write");
    d.editSupply(a,"voltageHighMv",14000);d.editSupply(a,"voltageHighMv",13200);
    require(!d.dirty(),"No-op supply changes omitted");
    d.editAdapterNames(a,d.adapterNames(a).at("groups"));require(!d.dirty(),"No-op labels omitted");
    d.editGroup(a,0,"faultDelaySeconds","");
    d.editGroup(b,1,"warningTemperatureDeciC","7");
    d.observeNano(a,snapshot(a));d.observeNano(b,snapshot(b));
    require(d.group(a,0).at("faultDelaySeconds")=="" && d.group(b,1).at("warningTemperatureDeciC")=="7",
            "Incomplete numeric inputs must survive navigation/reads without coercion");
    require(d.changes()[0].patch.at("faultDelaySeconds").is_string(),"Planner receives invalid input for validation");
    rejects([&]{d.editGroup(a,-1,"faultDelaySeconds",2);});
    rejects([&]{d.editGroup(a,2,"faultDelaySeconds",2);});
    rejects([&]{d.editGroup(a,0,"generation",2);});
    rejects([&]{d.editGroup(a,0,"mode","off");});
    rejects([&]{d.editGroup(a,0,"calibration",Json::object());});
    rejects([&]{d.editSupply(a,"hostUpdateTimeoutMs",20000);});
    rejects([&]{auto bad=d.host();bad["revision"]=99;d.editHost(bad);});
    rejects([&]{d.observeNano(a,snapshot(b));});
    rejects([&]{d.observeNano(std::string(32,'0'),snapshot(std::string(32,'0')));});
    rejects([&]{auto bad=snapshot(a);bad["configuration"]["generation"]=-1;d.observeNano(a,bad);});
    rejects([&]{auto bad=snapshot(a);bad["configuration"]["groups"]=Json::array();d.observeNano(a,bad);});
    require(d.group(a,0).at("faultDelaySeconds")=="","Failed reads leave existing edits intact");

    d = loaded();d.editGroup(a,0,"faultDelaySeconds",5);
    observed=snapshot(a);observed["configuration"]["adapterNames"]["generation"]=3;
    observed["configuration"]["adapterNames"]["groups"][0]["name"]="New label";d.observeNano(a,observed);
    require(d.conflicts().empty(),"Independent metadata generation must not block cooling changes");
    observed["calibration"][0]["generation"]=8;d.observeNano(a,observed);
    require(d.conflicts().size()==1,"Recalibration conflicts even without config generation changes");
    observed["status"]["calibrationActive"]=true;d.observeNano(a,observed);
    require(d.conflicts()[0].message.find("Calibration is active")!=std::string::npos,"Calibration blocks Nano saves");

    d = loaded();names=d.adapterNames(a).at("groups");names[0]["name"]="New";d.editAdapterNames(a,names);
    observed=snapshot(a);observed["configuration"]["generation"]=5;d.observeNano(a,observed);
    require(d.conflicts().empty(),"Cooling generation must not block independent metadata edit");
    observed["configuration"]["adapterNames"]["generation"]=3;d.observeNano(a,observed);
    require(d.conflicts().size()==1,"Metadata generation conflict detected");
    d.discardNano(a);d.observeNano(a,snapshot(a,false));
    require(d.adapterNames(a).is_null(),"Legacy firmware has unavailable labels, not an invented empty record");
    rejects([&]{d.editAdapterNames(a,names);});
    d.editGroup(a,0,"faultDelaySeconds",5);require(d.nanoDirty(a),"Legacy cooling edits remain possible");
    ConfigurationDraft empty;
    require(!empty.dirty() && empty.changes().empty(),"Empty workspace is clean");
    rejects([&]{empty.editHost(host());});
    rejects([&]{empty.editGroup(a,0,"faultDelaySeconds",5);});
    empty.discardAll();require(!empty.dirty(),"Empty discard is safe");
    d = loaded();h=d.host();h["controllers"][0]["groups"][0]={{"index",0},{"enabled",false},{"gpuPciAddress",nullptr}};
    d.editHost(h);
    require(d.hostDirty() && d.dirtyControllers()==0 && d.group(a,0).at("enabled")==true,
            "Clearing a mapping must not silently switch off Nano cooling");
    require(d.adapterNames(a)==snapshot(a).at("configuration").at("adapterNames"),
            "Automatic name proposals belong to save planning, not an unreviewed edit side effect");
    d.editHost(host());require(!d.dirty(),"Reverting host mapping removes the pending write");
    observed=snapshot(b);observed["configuration"]["groups"][1]["faultDelaySeconds"]=9;
    observed["configuration"]["generation"]=8;d.observeNano(b,observed);
    require(d.group(b,1).at("faultDelaySeconds")==9 && !d.nanoDirty(b),"Clean controller refresh adopts new values");
    d.editGroup(a,0,"faultDelaySeconds",5);observed=snapshot(a);
    observed["configuration"]["groups"][0]["criticalTemperatureDeciC"]=900;d.observeNano(a,observed);
    require(d.conflicts().size()==1,"Changed values with unchanged counters must not slip through conflict checks");
    rejects([&]{auto bad=host();bad["schemaVersion"]=1.0;d.observeHost(bad);});
    std::cout<<checks<<" configuration draft checks passed; no API or hardware accessed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
