#include "AlertCenter.hpp"
#include <iostream>
#include <syncstream>
#include <set>

namespace fan {
namespace {
std::int64_t nowMs() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
}
AlertCenter::Json AlertCenter::description(int scope,unsigned bit) {
    static const char* global[]={"INA monitor missing","Supply voltage low","Supply voltage critical","Supply voltage high","Default configuration","Invalid stored configuration","Host temperature timeout"};
    static const char* group[]={"Temperature invalid","Temperature warning","Temperature critical","Fan 1 slow/stopped","Fan 2 slow/stopped","Group current low","Group current high","Calibration aborted"};
    if(scope==3) return {{"message",bit==1?"Controller not responding":"Serial event history overflow"},{"severity","critical"}};
    unsigned index=0;while(index<16 && (1U<<index)!=bit) ++index;
    const auto known=scope==0?index<7:index<8;
    const bool critical=(bit&(scope==0?0x6DU:0x5DU))!=0;
    return {{"message",known?(scope==0?global[index]:group[index]):"Unknown Nano fault bit "+std::to_string(bit)},
        {"severity",critical?"critical":"warning"}};
}
bool AlertCenter::critical() const {
    for(const auto& [id,c]:controllers_) {
        (void)id;
        if(c.transportFault || c.historyLost || (c.masks[0]&0x6D) || ((c.masks[1]|c.masks[2])&0x5D)) return true;
    }
    return false;
}
void AlertCenter::record(const std::string& id,int scope,unsigned bit,bool raised,const char* source,std::int64_t at) {
    auto event=description(scope,bit);
    event.update({{"sequence",++sequence_},{"controllerId",id},{"scope",scope==0?"global":scope==3?"daemon":"group"},
        {"group",scope==1 || scope==2?Json(scope-1):Json(nullptr)},{"bit",bit},{"transition",raised?"raised":"cleared"},
        {"source",source},{"receivedAtMs",at}});
    if(history_.size()==256) history_.erase(0);
    history_.push_back(event);
    std::osyncstream(std::cerr)<<"Fan alert "<<event.dump()<<'\n';
    audible_.update(critical(),raised && event["severity"]=="critical");
}
void AlertCenter::configure(const Json& config,bool monitoring) {
    std::lock_guard lock(mutex_);
    std::set<std::string> wanted;
    if(monitoring) for(const auto& c:config.at("controllers")) {
        const auto id=c.at("controllerId").get<std::string>();wanted.insert(id);controllers_.try_emplace(id);
    }
    for(auto it=controllers_.begin();it!=controllers_.end();) {
        if(!wanted.contains(it->first)) it=controllers_.erase(it);else ++it;
    }
    audible_.update(critical(),false);
    audible_.enable(config.value("notifications",Json::object()).value("criticalBeep",false));
}
void AlertCenter::connection(const std::string& id,bool connected) {
    std::lock_guard lock(mutex_);
    const auto found=controllers_.find(id);if(found==controllers_.end()) return;
    auto& c=found->second;c.connected=connected;
    if(c.transportFault!=!connected) {
        c.transportFault=!connected;record(id,3,1,!connected,"daemon",nowMs());
    }
    audible_.update(critical(),false);
}
void AlertCenter::ingest(const std::string& id,const Json& batch) {
    std::lock_guard lock(mutex_);
    const auto found=controllers_.find(id);if(found==controllers_.end()) return;
    auto& c=found->second;
    if(batch.at("dropped").get<unsigned>() && !c.historyLost) {
        c.historyLost=true;record(id,3,2,true,"daemon",nowMs());
    }
    for(const auto& event:batch.at("events")) {
        const auto p=event.at("payload").get<std::vector<std::uint8_t>>();
        const int type=event.at("type");
        auto u16=[&](int i){return static_cast<unsigned>(p[i])|(static_cast<unsigned>(p[i+1])<<8);};
        std::array<unsigned,3> next{};
        const char* source;
        if(type==0x87 && p.size()==18) {
            bool valid=true;
            for(int s=0;s<3;++s) {
                const auto raised=u16(s*6),cleared=u16(s*6+2),active=u16(s*6+4);
                valid &= !(raised&cleared) && !(raised&~active) && !(cleared&active);
                next[s]=active;
            }
            if(!valid) continue;
            source="nano-alert";
        } else if(type==0x83 && p.size()==21 && p[10]<=7 && p[11]<=7 && p[20]<=1) {
            next={u16(4),u16(6),u16(8)};source="heartbeat-reconciliation";
        } else if(type==0x84 && p.size()==58 && p[10]<=1 && p[18]<=7 && p[38]<=7) {
            next={u16(6),u16(34),u16(54)};c.latched={u16(8),u16(36),u16(56)};source="status-reconciliation";
        } else continue;
        const auto at=event.at("receivedAtMs").get<std::int64_t>();
        for(int s=0;s<3;++s) {
            const auto changed=c.masks[s]^next[s];c.masks[s]=next[s];
            for(unsigned bit=1;bit<=0x8000;bit<<=1) if(changed&bit) record(id,s,bit,(next[s]&bit)!=0,source,at);
        }
        c.known=true;c.lastObserved=at;
    }
    audible_.update(critical(),false);
}
AlertCenter::Json AlertCenter::status() const {
    std::lock_guard lock(mutex_);
    Json states=Json::array(),active=Json::array();
    for(const auto& [id,c]:controllers_) {
        states.push_back({{"controllerId",id},{"known",c.known},{"connected",c.connected},{"stale",!c.connected},
            {"activeMasks",c.masks},{"lastStatusLatchedMasks",c.latched},{"lastObservedAtMs",c.lastObserved}});
        for(int scope=0;scope<4;++scope) {
            const auto mask=scope<3?c.masks[scope]:(c.transportFault?1U:0U)|(c.historyLost?2U:0U);
            for(unsigned bit=1;bit<=0x8000;bit<<=1) if(mask&bit) {
                auto fault=description(scope,bit);
                fault.update({{"controllerId",id},{"scope",scope==0?"global":scope==3?"daemon":"group"},
                    {"group",scope==1 || scope==2?Json(scope-1):Json(nullptr)},{"bit",bit},{"stale",scope<3 && !c.connected}});
                active.push_back(fault);
            }
        }
    }
    return {{"controllers",states},{"active",active},{"criticalActive",critical()},{"history",history_},
        {"latestSequence",sequence_},{"historyLimit",256},{"historyTruncated",sequence_>history_.size()},{"beeper",audible_.status()}};
}
}
