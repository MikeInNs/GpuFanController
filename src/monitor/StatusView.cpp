#include "StatusView.hpp"
#include <iomanip>
#include <sstream>
namespace fan {
using namespace ftxui;
namespace {
std::string value(const Json& j, double divisor=1) {
    if(j.is_null()) return "unavailable";
    std::ostringstream s; s<<std::fixed<<std::setprecision(divisor==1?0:2)<<j.get<double>()/divisor; return s.str();
}
std::string faults(int mask, bool global) {
    static const char* globalNames[]{"INA missing","voltage low","voltage critical","voltage high","default config","invalid stored data","host timeout"};
    static const char* groupNames[]{"temperature invalid","temperature warning","temperature critical","fan 1 slow","fan 2 slow","current low","current high","calibration aborted"};
    std::string result;
    for(int i=0;i<(global?7:8);++i) if(mask&(1<<i)) { if(!result.empty()) result+=", "; result+=global?globalNames[i]:groupNames[i]; }
    return result.empty()?"none":result;
}
}
Element statusView(const Json& snapshot,int group,bool live) {
    if(snapshot.is_null()) return text("No current snapshot. Select a discovered Nano and click Read Nano.")|dim;
    const auto& s=snapshot.at("status"); const auto& g=s.at("groups").at(group);
    const std::vector<std::string> modes{"disabled","automatic","manual","full","off","calibration","failsafe","ALARM"};
    const auto mode=g.at("mode").get<std::size_t>();
    return vbox({text("Nano mode: "+(mode<modes.size()?modes[mode]:"unknown"))|bold|color(mode>=6?Color::Red:Color::Cyan),
        text("Temperature: "+value(g["temperatureDeciC"],10)+" C   Target: "+value(g["targetRpm"])+" RPM   PWM: "+value(g["dutyPercent"])+"%"),
        separator(),text("Actual tach RPM   Fan 1: "+value(g["fanRpm"][0])+"     Fan 2: "+value(g["fanRpm"][1]))|bold,
        text("Group supply: "+value(g["voltageMv"],1000)+" V    Combined fan current: "+value(g["currentMa"],1000)+" A"),
        text("INA channel "+std::to_string(group+2)+"; individual fan current is not available."),
        separator(),paragraph("Controller alerts: "+faults(s["activeFaults"],true))|color(Color::Yellow),
        paragraph("Group alerts: "+faults(g["activeFaults"],false))|color(Color::Yellow),
        paragraph("Latched controller: "+faults(s["latchedFaults"],true)),
        paragraph("Latched group: "+faults(g["latchedFaults"],false)),
        text("Uptime: "+value(s["uptimeMs"],1000)+" s   Last host packet age: "+value(s["hostUpdateAgeMs"],1000)+" s"),
        paragraph(live?"Live readings only; configuration/calibration drafts are untouched. Switching pages stops these status reads.":"Read-only snapshot; use Read Nano to refresh. Check fanctl status --json for temperature-forwarding state and heartbeat health.")|dim});
}
Element calibrationView(const Json& snapshot,int group) {
    if(snapshot.is_null()) return text("Read the Nano to retrieve its stored calibration.")|dim;
    if(snapshot.at("status").at("calibrationActive").get<bool>() && snapshot.at("status").at("calibrationGroup")==group)
        return paragraph(snapshot.at("status").value("calibrationHardened",false)?
            "Measurements are in progress in a separate candidate. The last stored table stays active until verified saving. Results reload when calibration ends.":
            "Measurements are in progress. The completed table and measured RPM curve bounds will reload when calibration ends.")|color(Color::Yellow);
    const auto& c=snapshot.at("calibration").at(group);
    Elements rows{text(c.at("valid").get<bool>()?"Stored calibration (Nano measurements)":"No completed calibration stored")|bold|color(Color::Cyan),
        text("Generation: "+c.at("generation").dump()+"   Startup PWM: "+c.at("startDutyPercent")[0].dump()+"% / "+c.at("startDutyPercent")[1].dump()+"%"),
        text("PWM %    Fan 1 RPM    Fan 2 RPM    Group mA    Voltage mV")|bold};
    for(const auto& p:c.at("points")) {
        std::ostringstream line;
        line<<std::setw(5)<<value(p["dutyPercent"])<<std::setw(13)<<value(p["fanRpm"][0])<<std::setw(13)<<value(p["fanRpm"][1])
            <<std::setw(12)<<value(p["currentMa"])<<std::setw(14)<<value(p["voltageMv"]);
        rows.push_back(text(line.str()));
    }
    if(c.value("measuredMinimum",false)) {
        const int mask=snapshot.at("configuration").at("groups").at(group).at("expectedFanMask");
        const bool stop=(c.value("stopVerifiedMask",0)&mask)==mask;
        rows.push_back(text("Safe running floor: "+c.at("minimumRunningDutyPercent").dump()+"% PWM (includes 3 percentage-point margin)"));
        rows.push_back(paragraph(stop?"Both expected fans verified stopped at 0%. Zero-RPM curve points are available, not automatically enabled.":
            "Stop from PWM was not verified for every expected fan. No zero-RPM curve targets; 100% startup is conservative for a fan that cannot stop."));
    }
    rows.push_back(separator());
    rows.push_back(paragraph("Calibration intentionally stops fans. Keep the GPU idle and 12V power on. Startup is PWM %, not variable voltage; current is combined for both fans.")|color(Color::Yellow));
    rows.push_back(text("Replace fans or change wiring? Recalibrate before relying on this table.")|dim);
    return vbox(rows);
}
}
