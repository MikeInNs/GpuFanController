#include "LiveView.hpp"
#include "StatusView.hpp"
#include <iomanip>
#include <sstream>

namespace fan {
using namespace ftxui;
namespace {
std::string number(const nlohmann::json& n,int divisor=1) {
    if(n.is_null()) return "N/A";
    std::ostringstream s;s<<std::fixed<<std::setprecision(divisor==1?0:divisor==10?1:2)<<n.get<double>()/divisor;return s.str();
}
std::string age(const LiveMonitor::Sample* sample) {
    if(!sample || sample->status.is_null()) return "waiting for first reading";
    return "last reading "+std::to_string(std::chrono::duration_cast<std::chrono::seconds>(LiveMonitor::Clock::now()-sample->received).count())+"s ago";
}
std::string mode(int m) {
    const std::vector<std::string> names{"Disabled","Auto","Manual","Full","Off","Calibration","Failsafe","ALARM"};
    return m>=0 && m<static_cast<int>(names.size())?names[m]:"Unknown";
}
Element unavailable(const LiveMonitor::Sample* s) {
    return paragraph("UNAVAILABLE / STALE - "+(s && !s->error.empty()?s->error:age(s)))|color(Color::Yellow);
}
}
Element liveStatusView(const LiveMonitor::Sample* s,int group) {
    Elements rows{text("Live Status - refresh about every second while open")|bold};
    rows.push_back(text(age(s))|dim);
    if(!s || !s->fresh(LiveMonitor::Clock::now())) rows.push_back(unavailable(s));
    else rows.push_back(statusView({{"status",s->status}},group,true));
    return vbox(rows);
}
Element overviewView(const nlohmann::json& controllers,const LiveMonitor& monitor,bool available) {
    Elements rows{text("Live overview - all saved controllers")|bold,
        paragraph("Two concurrent reads; per-board sample ages are independent. Names/mappings are from the last host config reload.")|dim};
    if(!available) {rows.push_back(text("Update/reload the daemon for live monitoring."));return vbox(rows);}
    if(controllers.empty()) rows.push_back(paragraph("No saved controllers. Use Setup to discover, register and save mappings."));
    for(const auto& c:controllers) {
        const auto id=c.at("controllerId").get<std::string>();const auto* s=monitor.sample(id);
        rows.push_back(separator());
        rows.push_back(paragraph(c.at("name").get<std::string>()+" ["+id.substr(0,8)+"] - "+age(s))|bold|color(Color::Cyan));
        if(!s || !s->fresh(LiveMonitor::Clock::now())) {rows.push_back(unavailable(s));continue;}
        rows.push_back(text("Global faults: "+s->status.at("activeFaults").dump()+" | INA: "+(s->status.at("inaPresent").get<bool>()?"present":"unavailable")));
        for(int g=0;g<2;++g) {
            const auto& data=s->status.at("groups").at(g);
            const auto& mapping=c.at("groups").at(g);
            rows.push_back(paragraph("G"+std::to_string(g+1)+" "+mode(data.at("mode"))+" | "+number(data.at("temperatureDeciC"),10)+" C | PWM "+number(data.at("dutyPercent"))+"% | RPM "+number(data.at("fanRpm")[0])+" / "+number(data.at("fanRpm")[1]))|
                color(data.at("activeFaults")!=0?Color::Yellow:Color::White));
            rows.push_back(paragraph("   "+number(data.at("voltageMv"),1000)+" V / "+number(data.at("currentMa"),1000)+" A | faults "+data.at("activeFaults").dump()+" | GPU "+
                (mapping.at("enabled").get<bool>()?mapping.at("gpuPciAddress").get<std::string>():"unmapped (not Nano off)")));
        }
    }
    rows.push_back(separator());rows.push_back(paragraph("No individual fan current is measured. An unavailable/stale board never displays old RPM as current. Use Alerts for fault names/history.")|dim);
    return vbox(rows);
}
}
