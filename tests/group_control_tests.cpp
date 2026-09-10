#include "GroupControl.hpp"
#include "SettingsControl.hpp"
#include <iostream>
using Json=nlohmann::json;
void check(bool b){if(!b) throw std::runtime_error("Group control assertion failed");}
template<class F> void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
int main(){
    Json r{{"confirmed",true},{"mode","off"},{"timeoutSeconds",300},{"configGeneration",1}};
    check(fan::groupControl::modePayload(1,r)==std::vector<std::uint8_t>({1,3,0,44,1}));
    r["mode"]="full";check(fan::groupControl::modePayload(0,r)==std::vector<std::uint8_t>({0,2,100,44,1}));
    r["mode"]="auto";rejects([&]{fan::groupControl::validateMode(r);});r["timeoutSeconds"]=0;fan::groupControl::validateMode(r);
    const Json mutations{{"confirmed",false},{"mode","manual"},{"configGeneration",-1},{"timeoutSeconds",1.5}};
    for(const auto& [key,value]:mutations.items()) {
        auto bad=r;bad[key]=value;rejects([&]{fan::groupControl::validateMode(bad);});
    }
    r["mode"]="off";for(int n:{-1,0,3601}){r["timeoutSeconds"]=n;rejects([&]{fan::groupControl::validateMode(r);});}
    Json edit{{"configGeneration",1},{"calibrationGeneration",7},{"changes",{{"enabled",false}}}};
    rejects([&]{fan::groupControl::validateEdit(edit);});edit["confirmed"]=true;fan::groupControl::validateEdit(edit);
    edit["extra"]=true;rejects([&]{fan::groupControl::validateEdit(edit);});
    edit.erase("extra");edit.erase("confirmed");edit["changes"]={{"minimumDutyPercent",40}};
    rejects([&]{fan::groupControl::validateEdit(edit);});
    const Json scopes{{"group1",1},{"group2",2},{"global",128},{"all",131}};
    for(const auto& [scope,mask]:scopes.items())
        check(fan::settingsControl::clearMask({{"confirmed",true},{"scope",scope}})==mask.get<int>());
    for(const auto& request:std::vector<Json>{{{"confirmed",false},{"scope","all"}},{{"confirmed",true},{"scope","daemon"}},
        {{"confirmed",1},{"scope","all"}},{{"confirmed",true},{"scope","all"},{"mask",255}}})
        rejects([&]{fan::settingsControl::clearMask(request);});
    Json supply{{"confirmed",true},{"configGeneration",1},{"changes",{{"voltageHighMv",13200}}}};
    fan::settingsControl::validateSupply(supply);
    supply["changes"]["hostUpdateTimeoutMs"]=5000;rejects([&]{fan::settingsControl::validateSupply(supply);});
    std::cout<<"Group control confirmations, strict mode/timeout validation and payloads passed\n";
}
