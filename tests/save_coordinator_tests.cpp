#include "SaveCoordinator.hpp"
#include "ControllerData.hpp"
#include <iostream>
#include <stdexcept>
using Json=nlohmann::json;
using namespace fan;
namespace {
const std::string a(32,'a'),b(32,'b');int checks=0;
void check(bool ok,const char* why) {++checks;if(!ok) throw std::runtime_error(why);}
template<class F> void rejects(F action) {bool threw=false;try{action();}catch(const std::exception&){threw=true;}check(threw,"Expected rejection");}
Json nano(const std::string& id) {
    Json g{{"enabled",true},{"expectedFanMask",3},{"minimumDutyPercent",20},{"startupDutyPercent",100},
        {"startupTimeMs",1000},{"warningTemperatureDeciC",750},{"criticalTemperatureDeciC",850},
        {"rpmLowThresholdPercent",60},{"faultDelaySeconds",3},{"currentDeviationPercent",40},
        {"curve",Json::array({{{"temperatureDeciC",300},{"rpm",2500}},{{"temperatureDeciC",850},{"rpm",8500}}})}};
    Json c{{"generation",7},{"valid",true},{"measuredMinimum",false},{"startDutyPercent",{15,15}},
        {"minimumRunningDutyPercent",20},{"points",Json::array()}};
    for(int duty=20;duty<=100;duty+=10) c["points"].push_back({{"dutyPercent",duty},{"fanRpm",{duty*100,duty*90}},
        {"voltageMv",12000},{"currentMa",duty*4}});
    return {{"controllerId",id},{"groupControlsAvailable",true},
        {"configuration",{{"generation",1},{"hostUpdateTimeoutMs",10000},{"voltageCriticalLowMv",10500},
            {"voltageWarningLowMv",11000},{"voltageHighMv",13200},{"groups",Json::array({g,g})},
            {"adapterNames",{{"generation",2},{"groups",Json::array({
                {{"pciAddress",id==a?"0000:01:00.0":"0000:02:00.0"},{"name",id==a?"GPU A":"GPU B"}},
                {{"pciAddress",nullptr},{"name",""}}})}}}}},
        {"calibration",Json::array({c,c})},{"status",{{"calibrationActive",false}}}};
}
struct Fixture {
    Json host{{"schemaVersion",1},{"revision",3},{"controllers",Json::array()}},inventory{{"gpus",Json::array()}};
    std::map<std::string,Json> boards{{a,nano(a)},{b,nano(b)}};
    std::vector<std::pair<DraftScope,std::string>> writes;
    std::string lost,offline;bool hostLost=false,wrongReply=false;
    Fixture() {for(const auto& id:{a,b}) {
        const std::string pci=id==a?"0000:01:00.0":"0000:02:00.0";
        host["controllers"].push_back({{"controllerId",id},{"name",id==a?"A":"B"},{"groups",Json::array({
            {{"index",0},{"enabled",true},{"gpuPciAddress",pci}},{{"index",1},{"enabled",false},{"gpuPciAddress",nullptr}}})}});
        inventory["gpus"].push_back({{"pciAddress",pci},{"name",id==a?"GPU A":"GPU B"}});
    }}
    ConfigurationDraft draft() const {ConfigurationDraft d;d.observeHost(host);for(const auto& [id,s]:boards)d.observeNano(id,s);return d;}
    SaveIo io() {return {[this]{return host;},[this](const auto& id){if(id==offline)throw std::runtime_error("offline");return boards.at(id);},
        [this](DraftScope scope,const std::string& id,const Json& req)->Json {
            writes.emplace_back(scope,id);
            if(scope==DraftScope::Host) {check(req["revision"]==host["revision"],"Host revision");host=req;host["revision"]=host["revision"].get<int>()+1;
                if(hostLost)throw std::runtime_error("Host reply lost");
                return host;}
            auto& s=boards.at(id);auto& c=s["configuration"];
            if(scope==DraftScope::AdapterNames) {
                check(req["adapterNames"]["generation"]==c["adapterNames"]["generation"],"Name generation");
                c["adapterNames"]=req["adapterNames"];c["adapterNames"]["generation"]=c["adapterNames"]["generation"].get<int>()+1;
            } else {
                check(req["configGeneration"]==c["generation"],"Fresh generation between scopes");
                if(scope==DraftScope::Supply)c.update(req.at("changes"));
                else {int g=scope==DraftScope::Group1?0:1;
                    check(req["calibrationGeneration"]==s["calibration"][g]["generation"],"Calibration counter");
                    c["groups"][g].update(req.at("changes"));}
                c["generation"]=c["generation"].get<int>()+1;
            }
            if(id==lost)throw std::runtime_error("EEPROM save/reply unconfirmed");
            auto response=s;if(wrongReply)response["configuration"]["groups"][1]["faultDelaySeconds"]=29;
            return response;
        }};}
};
}
int main() try {
    Fixture f;SaveCoordinator save(f.io());auto d=f.draft();
    check(save.prepare(d,f.inventory).operations.empty(),"No-op must produce no writes");
    auto h=d.host();h["notifications"]={{"criticalBeep",true}};d.editHost(h);
    d.editGroup(a,0,"warningTemperatureDeciC","700");d.editGroup(a,1,"faultDelaySeconds",5);
    d.editSupply(a,"voltageHighMv",13000);d.editGroup(b,0,"startupTimeMs",2000);
    f.inventory["gpus"][0]["name"]="GPU A updated";
    auto p=save.prepare(d,f.inventory);
    check(f.writes.empty() && p.operations.size()==6,"Preflight reads only and includes automatic names");
    check(p.review.find("70 C")!=std::string::npos && p.review.find("motherboard")!=std::string::npos,"Review units and audible risk");
    auto r=save.execute(p);
    check(f.writes.size()==6 && f.writes.back().first==DraftScope::Host,"Nano writes precede host activation");
    check(!r.draft.dirty() && r.unconfirmed.empty(),"All verified scopes clear");
    check(f.boards[a]["configuration"]["generation"]==4,"Both groups and supply use sequential generations");
    check(save.prepare(r.draft,f.inventory).operations.empty(),"Verified scopes never need resending");

    Fixture partial;SaveCoordinator run(partial.io());d=partial.draft();
    h=d.host();h["notifications"]={{"criticalBeep",true}};d.editHost(h);
    d.editGroup(a,0,"faultDelaySeconds",4);d.editGroup(b,1,"faultDelaySeconds",5);
    partial.lost=b;p=run.prepare(d,partial.inventory);r=run.execute(p);
    check(partial.writes.size()==2 && r.unconfirmed.size()==1,"Lost Nano reply prevents dependent host write");
    check(!r.draft.nanoDirty(a) && r.draft.nanoDirty(b) && r.draft.hostDirty(),"Successful scopes clear, remaining intent retained");
    partial.lost.clear();auto retry=run.prepare(r.draft,partial.inventory,r.unconfirmed);
    check(retry.operations.size()==2,"Retry retains a matching RAM value until persistence verified");
    auto recovered=run.execute(retry);
    check(partial.writes.size()==4 && partial.writes[2].second==b && partial.writes[3].first==DraftScope::Host,
          "Only unconfirmed Nano and pending host are retried");
    check(!recovered.draft.dirty() && recovered.unconfirmed.empty(),"Retry verified and cleared");

    Fixture conflict;SaveCoordinator cs(conflict.io());d=conflict.draft();d.editGroup(a,0,"faultDelaySeconds",5);
    p=cs.prepare(d,conflict.inventory);conflict.host["revision"]=4;
    rejects([&]{cs.execute(p);});check(conflict.writes.empty(),"Host race after review writes nothing");
    conflict.host["revision"]=3;conflict.boards[a]["configuration"]["generation"]=2;
    rejects([&]{cs.prepare(d,conflict.inventory);});check(conflict.writes.empty(),"Nano conflict preflight writes nothing");

    Fixture invalid;SaveCoordinator vs(invalid.io());d=invalid.draft();
    d.editGroup(a,0,"faultDelaySeconds",5);d.editGroup(b,1,"faultDelaySeconds","");
    rejects([&]{vs.prepare(d,invalid.inventory);});check(invalid.writes.empty(),"All draft validation before mutation");
    d=invalid.draft();auto curve=d.group(a,0).at("curve");curve[0]["rpm"]=2600;
    d.editGroup(a,0,"curve",curve);d.editGroup(a,0,"expectedFanMask",1);
    rejects([&]{vs.prepare(d,invalid.inventory);});
    d=invalid.draft();d.editSupply(a,"voltageHighMv",10000);rejects([&]{vs.prepare(d,invalid.inventory);});
    d=invalid.draft();d.editGroup(a,0,"faultDelaySeconds",5);invalid.boards[a]["status"]["calibrationActive"]=true;
    rejects([&]{vs.prepare(d,invalid.inventory);});check(invalid.writes.empty(),"Calibration blocks save");

    Fixture off;SaveCoordinator os(off.io());d=off.draft();d.editGroup(a,0,"faultDelaySeconds",4);d.editGroup(b,0,"faultDelaySeconds",5);
    off.offline=b;p=os.prepare(d,off.inventory);r=os.execute(p);
    check(off.writes.size()==1 && r.draft.nanoDirty(b) && !r.draft.nanoDirty(a),"Independent online controller can complete");
    check(r.report.find("NOT ATTEMPTED")!=std::string::npos,"Offline outcome is explicit");

    Fixture hr;SaveCoordinator hs(hr.io());d=hr.draft();h=d.host();h["notifications"]={{"criticalBeep",true}};d.editHost(h);
    hr.hostLost=true;r=hs.execute(hs.prepare(d,hr.inventory));check(r.unconfirmed.size()==1,"Host lost reply retained");
    hr.hostLost=false;r=hs.execute(hs.prepare(r.draft,hr.inventory,r.unconfirmed));
    check(!r.draft.dirty() && r.unconfirmed.empty() && hr.writes.size()==2,"Host uncertainty reconciled and explicitly rewritten");

    Fixture mismatch;SaveCoordinator ms(mismatch.io());d=mismatch.draft();d.editGroup(a,0,"faultDelaySeconds",5);
    mismatch.wrongReply=true;r=ms.execute(ms.prepare(d,mismatch.inventory));
    check(r.unconfirmed.size()==1 && r.draft.nanoDirty(a),"Mismatched readback cannot be reported saved");
    mismatch.wrongReply=false;mismatch.boards[a]["configuration"]["groups"][1]["faultDelaySeconds"]=10;
    p=ms.prepare(r.draft,mismatch.inventory,r.unconfirmed);
    check(p.blocked.contains(a),"Unrelated external change cannot be silently rebased on retry");
    auto count=mismatch.writes.size();ms.execute(p);check(mismatch.writes.size()==count,"Conflict retry performs no write");

    Fixture legacy;legacy.boards[a]["configuration"].erase("adapterNames");SaveCoordinator ls(legacy.io());d=legacy.draft();
    d.editGroup(a,0,"faultDelaySeconds",5);p=ls.prepare(d,legacy.inventory);check(p.review.find("legacy")!=std::string::npos,"Unsupported names explained");
    r=ls.execute(p);check(!r.draft.dirty(),"Legacy cosmetic limitation does not prevent cooling save");
    std::cout<<checks<<" save coordinator checks passed; fake transports only\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
