#include "SaveCoordinator.hpp"
#include "ConfigStore.hpp"
#include "ControllerData.hpp"
#include "AdapterNames.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>

namespace fan {
namespace {
using Json=nlohmann::json;
bool same(const DraftChange& a,const DraftChange& b) {return a.controllerId==b.controllerId && a.scope==b.scope;}
Json settings(const Json& s) {return {{"configuration",s.at("configuration")},{"calibration",s.at("calibration")}};}
Json withoutRevision(Json j) {j.erase("revision");return j;}
Json section(const Json& s,DraftScope scope) {
    const auto& c=s.at("configuration");
    if(scope==DraftScope::AdapterNames) return c.at("adapterNames");
    if(scope==DraftScope::Supply) return {{"voltageCriticalLowMv",c.at("voltageCriticalLowMv")},
        {"voltageWarningLowMv",c.at("voltageWarningLowMv")},{"voltageHighMv",c.at("voltageHighMv")}};
    return c.at("groups")[scope==DraftScope::Group1?0:1];
}
Json normalized(Json patch) {
    for(auto& [key,value]:patch.items()) if(value.is_string() && key!="enabled") {
        auto text=value.get<std::string>();int number=0;
        auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),number);
        if(ec!=std::errc{} || end!=text.data()+text.size()) throw std::invalid_argument(key+": enter a whole number in the labelled units");
        value=number;
    }
    return patch;
}
void stage(ConfigurationDraft& draft,const DraftChange& c) {
    if(c.scope==DraftScope::Host) draft.editHost(c.after);
    else if(c.scope==DraftScope::AdapterNames) draft.editAdapterNames(c.controllerId,c.patch.at("groups"));
    else for(const auto& [key,value]:c.patch.items()) {
        if(c.scope==DraftScope::Supply) draft.editSupply(c.controllerId,key,value);
        else draft.editGroup(c.controllerId,c.scope==DraftScope::Group1?0:1,key,value);
    }
}
void validate(const DraftChange& c,const Json& snapshot) {
    try {
        if(c.scope==DraftScope::Host) {ConfigStore::validate(c.after);return;}
        if(snapshot.at("status").at("calibrationActive")==true) throw std::invalid_argument("Finish or abort calibration first");
        if(c.scope==DraftScope::AdapterNames) {adapterNames::encode(c.after);return;}
        const auto& config=snapshot.at("configuration");
        const auto bytes=controller::configurationBytes(config);
        if(c.scope==DraftScope::Supply) {
            if(!snapshot.value("groupControlsAvailable",false)) throw std::invalid_argument("Firmware 1.3.0 or newer required");
            controller::editSupply(bytes,c.patch);
        } else {
            const int g=c.scope==DraftScope::Group1?0:1;
            if(!snapshot.value("groupControlsAvailable",false)) for(const auto& [key,value]:c.patch.items()) {
                (void)value;
                if(key=="enabled" || key=="expectedFanMask" || key=="minimumDutyPercent" || key=="startupDutyPercent" || key=="startupTimeMs")
                    throw std::invalid_argument("Firmware 1.3.0 or newer required");
            }
            controller::editConfiguration(bytes,g,c.patch,snapshot.at("calibration")[g]);
        }
    } catch(const std::exception& e) {throw std::invalid_argument(SaveCoordinator::label(c)+": "+e.what());}
}
// A retry can reconcile only the previous uncertain operation's own changes.
// Anything else still requires explicit conflict resolution, never blind rebase.
bool retryCompatible(const Json& before,const Json& fresh,const std::vector<DraftChange>& retry,const std::string& id) {
    auto old=before.at("configuration"),now=fresh.at("configuration");
    std::set<int> topology;
    for(const auto& c:retry) if(c.controllerId==id) {
        if(c.scope==DraftScope::AdapterNames) {
            if(!now.contains("adapterNames")) return false;
            if(now["adapterNames"]["groups"]==c.patch.at("groups")) old["adapterNames"]["groups"]=c.patch.at("groups");
        } else for(const auto& [key,value]:c.patch.items()) {
            if(c.scope==DraftScope::Supply) {if(now.at(key)==value) old[key]=value;}
            else {int g=c.scope==DraftScope::Group1?0:1;
                if(now["groups"][g].at(key)==value) old["groups"][g][key]=value;
                if(key=="expectedFanMask") topology.insert(g);
            }
        }
    }
    old.erase("generation");now.erase("generation");
    if(old.contains("adapterNames") && now.contains("adapterNames")) {
        old["adapterNames"].erase("generation");now["adapterNames"].erase("generation");
    }
    if(old!=now) return false;
    for(int g=0;g<2;++g) if(!topology.contains(g)) {
        auto a=before.at("calibration")[g],b=fresh.at("calibration")[g];
        a.erase("generation");b.erase("generation");a.erase("rpmRange");b.erase("rpmRange");
        if(a!=b) return false;
    }
    return true;
}
std::string value(const std::string& key,const Json& j) {
    if(j.is_number() && (key=="warningTemperatureDeciC" || key=="criticalTemperatureDeciC")) {
        std::ostringstream o;o<<j.get<double>()/10<<" C";return o.str();
    }
    std::string result=j.dump();
    if(key.find("Mv")!=std::string::npos) result+=" mV";
    else if(key.find("Percent")!=std::string::npos) result+="%";
    else if(key=="startupTimeMs") result+=" ms";
    else if(key=="faultDelaySeconds") result+=" s";
    return result;
}
std::string friendly(const std::string& key) {
    const std::map<std::string,std::string> names{{"enabled","Group enabled"},{"expectedFanMask","Expected fan mask (1=fan1, 2=fan2, 3=both, 0=none)"},
        {"minimumDutyPercent","Minimum PWM"},{"startupDutyPercent","Startup PWM"},{"startupTimeMs","Startup duration"},
        {"warningTemperatureDeciC","Warning temperature"},{"criticalTemperatureDeciC","Critical temperature"},
        {"rpmLowThresholdPercent","Low RPM threshold"},{"faultDelaySeconds","Fault delay"},{"currentDeviationPercent","Current deviation"},
        {"voltageCriticalLowMv","Critical-low supply"},{"voltageWarningLowMv","Warning-low supply"},{"voltageHighMv","High supply"}};
    return names.contains(key)?names.at(key):key;
}
std::string pciLabel(const Json& value) {return value.is_null()?"none":value.get<std::string>();}
std::string hostReview(const Json& before,const Json& after) {
    std::string text;
    for(const auto& c:after.at("controllers")) {
        const auto id=c.at("controllerId").get<std::string>();Json old=nullptr;
        for(const auto& previous:before.at("controllers")) if(previous.at("controllerId")==id) old=previous;
        if(old.is_null()) text+="Add controller "+c.at("name").get<std::string>()+" ["+id+"]\n";
        else if(old.at("name")!=c.at("name")) text+="Controller name: "+old.at("name").get<std::string>()+" -> "+c.at("name").get<std::string>()+"\n";
        for(int g=0;g<2;++g) if(old.is_null() || old.at("groups")[g]!=c.at("groups")[g])
            text+=c.at("name").get<std::string>()+" group "+std::to_string(g+1)+" GPU: "+
                (old.is_null()?"not configured":pciLabel(old.at("groups")[g].at("gpuPciAddress")))+" -> "+pciLabel(c.at("groups")[g].at("gpuPciAddress"))+"\n";
    }
    for(const auto& old:before.at("controllers")) if(std::none_of(after.at("controllers").begin(),after.at("controllers").end(),
        [&](const auto& c){return c.at("controllerId")==old.at("controllerId");})) text+="Remove controller mapping: "+old.at("name").get<std::string>()+" (does not turn Nano off)\n";
    const bool a=before.value("notifications",Json::object()).value("criticalBeep",false),b=after.value("notifications",Json::object()).value("criticalBeep",false);
    if(a!=b) text+=std::string("Motherboard beeper: ")+(a?"ON":"OFF")+" -> "+(b?"ON":"OFF")+"\n";
    return text;
}
}
SaveIo SaveIo::api(const ApiClient& client) {
    return {[client]{return client.config();},[client](const auto& id){return client.snapshot(id);},
        [client](DraftScope s,const std::string& id,const Json& request) {
            if(s==DraftScope::Host) return client.save(request);
            if(s==DraftScope::Supply) return client.updateSupply(id,request);
            if(s==DraftScope::AdapterNames) return client.updateAdapterNames(id,request);
            return client.updateGroup(id,s==DraftScope::Group1?0:1,request);
        }};
}
std::string SaveCoordinator::label(const DraftChange& c) {
    if(c.scope==DraftScope::Host) return "Daemon configuration";
    const std::string scope=c.scope==DraftScope::Group1?"group 1":c.scope==DraftScope::Group2?"group 2":
        c.scope==DraftScope::Supply?"supply (both groups)":"GPU names";
    return "Nano "+c.controllerId+" / "+scope;
}
SavePlan SaveCoordinator::prepare(ConfigurationDraft draft,const Json& inventory,const std::vector<DraftChange>& retry) const {
    SavePlan p;p.draft=std::move(draft);p.unconfirmed=retry;
    ConfigStore::validate(p.draft.host());
    p.hostRead=io_.host();ConfigStore::validate(p.hostRead);
    if(retry.empty() && p.draft.dirtyControllers() && p.hostRead!=p.draft.latestHost())
        throw std::invalid_argument("Host mappings/preferences changed while editing Nano settings. Reload to inspect them before saving.");
    const auto hostRetry=std::find_if(retry.begin(),retry.end(),[](const auto& c){return c.scope==DraftScope::Host;});
    if(hostRetry!=retry.end()) {
        if(withoutRevision(p.hostRead)!=withoutRevision(hostRetry->before) && withoutRevision(p.hostRead)!=withoutRevision(hostRetry->after))
            throw std::invalid_argument("Daemon config changed outside the unconfirmed save; resolve the conflict before retrying");
        p.draft.rebaseHost(p.hostRead);
    } else p.draft.observeHost(p.hostRead);
    std::set<std::string> targets;
    for(const auto& c:p.draft.changes()) if(c.scope!=DraftScope::Host) targets.insert(c.controllerId);
    for(const auto& c:retry) if(c.scope!=DraftScope::Host) targets.insert(c.controllerId);
    // Synchronize names for newly/remapped controllers and already-read boards.
    for(const auto& c:p.draft.host().at("controllers")) {
        const std::string id=c.at("controllerId");bool changed=true;
        for(const auto& old:p.hostRead.at("controllers")) if(old.at("controllerId")==id) changed=old.at("groups")!=c.at("groups");
        if(changed || p.draft.hasNano(id)) targets.insert(id);
    }
    for(const auto& id:targets) {
        try {
            auto fresh=io_.snapshot(id);
            ConfigurationDraft check;check.observeNano(id,fresh); // Full shape/identity validation before accepting.
            const bool uncertain=std::any_of(retry.begin(),retry.end(),[&](const auto& c){return c.controllerId==id;});
            if(uncertain && p.draft.hasNano(id)) {
                if(!retryCompatible(p.draft.baselineNano(id),fresh,retry,id)) throw std::invalid_argument("Settings changed outside the uncertain write; discard/review this conflict first");
                p.draft.rebaseNano(id,fresh);
            } else p.draft.observeNano(id,fresh);
            p.snapshots[id]=fresh;
        } catch(const std::exception& e) {p.blocked[id]=e.what();}
    }
    for(const auto& c:p.draft.host().at("controllers")) {
        const std::string id=c.at("controllerId");if(!p.snapshots.contains(id)) continue;
        auto names=p.draft.adapterNames(id);
        if(names.is_null()) {p.review+="Nano "+id+": legacy firmware; GPU labels cannot be stored.\n";continue;}
        for(int g=0;g<2;++g) {
            const auto pci=c.at("groups")[g].at("gpuPciAddress");std::string name;
            if(!pci.is_null()) {
                for(const auto& gpu:inventory.at("gpus")) if(gpu.at("pciAddress")==pci) name=gpu.at("name");
                if(name.empty() && names["groups"][g]["pciAddress"]==pci) name=names["groups"][g]["name"];
                if(name.empty()) {
                    // Existing legacy/empty labels need a scan, but must not prevent an unrelated cooling save.
                    bool mappingChanged=true;
                    for(const auto& old:p.hostRead.at("controllers")) if(old.at("controllerId")==id)
                        mappingChanged=old.at("groups")[g].at("gpuPciAddress")!=pci;
                    if(mappingChanged) throw std::invalid_argument("Scan GPUs to obtain the new mapped adapter's name");
                    p.review+="Nano "+id+": name unavailable; scan GPUs to store a label.\n";continue;
                }
                for(char& ch:name) if(static_cast<unsigned char>(ch)<32 || static_cast<unsigned char>(ch)>126) ch='?';
                if(name.size()>31) name=name.substr(0,28)+"...";
            }
            names["groups"][g]={{"pciAddress",pci},{"name",name}};
        }
        p.draft.editAdapterNames(id,names.at("groups"));
    }
    for(const auto& conflict:p.draft.conflicts())
        if(conflict.controllerId.empty() || !p.blocked.contains(conflict.controllerId))
            throw std::invalid_argument(conflict.controllerId+": "+conflict.message+". Read/review and explicitly discard conflicting edits before saving.");
    p.operations=p.draft.changes();
    // A matching RAM observation is not proof of persistence. Force explicitly
    // confirmed retries even when the observed values now equal the desired ones.
    for(const auto& c:retry) {
        auto found=std::find_if(p.operations.begin(),p.operations.end(),[&](const auto& x){return same(x,c);});
        if(found==p.operations.end()) p.operations.push_back(c);
        else if(c.scope!=DraftScope::Host) {auto patch=c.patch;patch.update(found->patch);found->patch=patch;}
    }
    for(auto& c:p.operations) {
        if(c.scope==DraftScope::Host) {c.before=p.hostRead;c.after=p.draft.host();c.after["revision"]=p.hostRead.at("revision");}
        else {
            if(c.scope!=DraftScope::AdapterNames) c.patch=normalized(c.patch);
            const auto& snap=p.snapshots.contains(c.controllerId)?p.snapshots.at(c.controllerId):p.draft.baselineNano(c.controllerId);
            c.before=section(snap,c.scope);c.after=c.before;c.after.update(c.patch);
            validate(c,snap);stage(p.draft,c);
        }
        p.review+="\n"+label(c)+"\n";
        if(c.scope==DraftScope::Host) p.review+=hostReview(c.before,c.after);
        else if(c.scope==DraftScope::AdapterNames) {
            for(int g=0;g<2;++g) if(c.before.at("groups")[g]!=c.after.at("groups")[g]) {
                const auto& old=c.before.at("groups")[g];const auto& next=c.after.at("groups")[g];
                p.review+="Group "+std::to_string(g+1)+" label: "+pciLabel(old.at("pciAddress"))+" / "+old.at("name").get<std::string>()+
                    " -> "+pciLabel(next.at("pciAddress"))+" / "+next.at("name").get<std::string>()+"\n";
            }
        } else for(const auto& [key,v]:c.patch.items()) {
            if(key=="curve") {
                p.review+="Curve points (temperature C / target RPM):\n";
                for(std::size_t i=0;i<v.size();++i) {
                    const auto point=[](const Json& p){return value("warningTemperatureDeciC",p.at("temperatureDeciC"))+" / "+p.at("rpm").dump()+" RPM";};
                    p.review+="  "+std::to_string(i+1)+": "+(i<c.before.at("curve").size()?point(c.before.at("curve")[i]):"new point")+" -> "+point(v[i])+"\n";
                }
            } else p.review+=friendly(key)+": "+value(key,c.before.at(key))+" -> "+value(key,v)+"\n";
        }
        if(c.patch.contains("expectedFanMask")) p.review+="WARNING: changing expected fans clears this group's calibration. Recalibrate; never hide a failed required fan.\n";
        if(c.scope!=DraftScope::Host && c.patch.value("enabled",true)==false) p.review+="WARNING: disabling removes local thermal/tach/current protection. 0% PWM is not a 12V cut; host timeout still forces full speed.\n";
        if(c.patch.contains("curve")) for(const auto& point:c.patch.at("curve")) if(point.at("rpm")==0) {p.review+="WARNING: zero RPM explicitly enables fan-stop. Verify safe idle cooling.\n";break;}
        if(c.patch.contains("startupDutyPercent") || c.patch.contains("startupTimeMs")) p.review+="WARNING: low startup PWM/short boost can stall fans. Supervise startup.\n";
        if(c.scope==DraftScope::Supply) p.review+="WARNING: relaxed supply thresholds may hide power problems.\n";
    }
    if(p.draft.host().value("notifications",Json::object()).value("criticalBeep",false) &&
       !p.hostRead.value("notifications",Json::object()).value("criticalBeep",false))
        p.review+="WARNING: enabling the motherboard beeper can sound immediately for an active critical fault.\n";
    for(const auto& [id,error]:p.blocked) p.review+="\nBLOCKED Nano "+id+": "+error+". Dependent daemon changes will remain pending.\n";
    if(!retry.empty()) p.review+="\nUnconfirmed writes will be explicitly retried after this readback/review; matching RAM values alone do not prove EEPROM storage.\n";
    p.review+="\nConfirm saves changed scopes only. Results may be partial; no automatic rollback/retry. Host mappings do not enable/disable Nano outputs. Temperature forwarding continues.";
    return p;
}
SaveResult SaveCoordinator::execute(const SavePlan& p) const {
    SaveResult r{p.draft,p.unconfirmed,{}};auto snapshots=p.snapshots;auto blocked=p.blocked;
    if(io_.host()!=p.hostRead) throw std::invalid_argument("Daemon configuration changed after review. Nothing saved; review again.");
    auto verified=[&](const DraftChange& c){std::erase_if(r.unconfirmed,[&](const auto& x){return same(x,c);});};
    for(const auto& c:p.operations) if(c.scope!=DraftScope::Host) {
        if(blocked.contains(c.controllerId)) {r.report+=label(c)+": NOT ATTEMPTED - "+blocked.at(c.controllerId)+"\n";continue;}
        bool sent=false;
        try {
            const auto fresh=io_.snapshot(c.controllerId);
            if(fresh.at("controllerId")!=c.controllerId || settings(fresh)!=settings(snapshots.at(c.controllerId)))
                throw std::invalid_argument("Settings changed after review; nothing written for this scope");
            validate(c,fresh);
            Json request{{"confirmed",true}};auto expected=fresh.at("configuration");
            if(c.scope==DraftScope::AdapterNames) {
                auto names=expected.at("adapterNames");names["groups"]=c.patch.at("groups");request["adapterNames"]=names;
                expected["adapterNames"]=names;
            } else {
                request["configGeneration"]=expected.at("generation");request["changes"]=c.patch;
                if(c.scope==DraftScope::Supply) expected.update(c.patch);
                else {int g=c.scope==DraftScope::Group1?0:1;expected["groups"][g].update(c.patch);
                    request["calibrationGeneration"]=fresh.at("calibration")[g].at("generation");}
                expected["generation"]=expected.at("generation").get<std::uint32_t>()+std::uint32_t{1};
            }
            sent=true;auto result=io_.write(c.scope,c.controllerId,request);
            ConfigurationDraft check;check.observeNano(c.controllerId,result);
            if(c.scope==DraftScope::AdapterNames) {
                const auto old=expected["adapterNames"]["generation"].get<std::uint32_t>();
                const auto next=result.at("configuration").at("adapterNames").at("generation").get<std::uint32_t>();
                if(next!=old && next!=old+std::uint32_t{1}) throw std::runtime_error("Unexpected label generation");
                expected["adapterNames"]["generation"]=next;
            }
            if(result.at("configuration")!=expected) throw std::runtime_error("Configuration readback mismatch");
            snapshots[c.controllerId]=result;r.draft.acceptNano(c.controllerId,result,c.scope);verified(c);
            r.report+=label(c)+": SAVED - verified by readback\n";
        } catch(const std::exception& e) {
            // Even an HTTP error may have arisen during post-write readback.
            // Once submitted, conservatively retain uncertainty until verified.
            const bool uncertain=sent;
            if(uncertain && std::none_of(r.unconfirmed.begin(),r.unconfirmed.end(),[&](const auto& x){return same(x,c);})) r.unconfirmed.push_back(c);
            blocked[c.controllerId]=e.what();r.report+=label(c)+(uncertain?": UNCONFIRMED - ":": NOT SAVED - ")+e.what()+"\n";
        }
    }
    for(const auto& c:p.operations) if(c.scope==DraftScope::Host) {
        if(!blocked.empty()) {r.report+="Daemon: NOT ATTEMPTED - a Nano prerequisite did not complete. Host changes remain pending.\n";continue;}
        bool sent=false;
        try {
            if(io_.host()!=p.hostRead) throw std::invalid_argument("Daemon configuration changed during save");
            sent=true;auto saved=io_.write(c.scope,{},c.after);auto expected=c.after;
            expected["revision"]=c.after.at("revision").get<int>()+1;
            if(saved!=expected || io_.host()!=saved) throw std::runtime_error("Daemon readback mismatch");
            r.draft.acceptHost(saved);verified(c);r.report+="Daemon: SAVED - verified by readback\n";
        } catch(const std::exception& e) {
            const bool uncertain=sent;
            if(uncertain && std::none_of(r.unconfirmed.begin(),r.unconfirmed.end(),[&](const auto& x){return same(x,c);})) r.unconfirmed.push_back(c);
            r.report+=std::string("Daemon: ")+(uncertain?"UNCONFIRMED - ":"NOT SAVED - ")+e.what()+"\n";
        }
    }
    r.report+="\nUnsaved edits remain pending. Unconfirmed means a write may have happened; Save changes reads/reviews before an explicitly confirmed retry. Discard does not undo writes.";
    return r;
}
}
