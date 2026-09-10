#pragma once
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

namespace fan {
inline ftxui::Element calibrationProgressView(const nlohmann::json& snapshot) {
    using namespace ftxui;
    if(snapshot.is_null()) return text("Read Nano before starting or monitoring calibration.")|dim;
    const auto& s=snapshot.at("status");
    const int phase=s.at("calibrationPhase");
    const bool active=s.at("calibrationActive");
    const bool hardened=s.value("calibrationHardened",false);
    const std::vector<std::string> phases{"Idle","PWM sweep","Spin-down","Startup search",hardened?"Saving measurements":"Complete (storage unverified)","Aborted",
        s.value("calibrationStorageConfirmed",nlohmann::json(nullptr))==true?"Saved - EEPROM verified":"Storage unverified",
        "Storage failed","Unstable RPM","Invalid fan measurements","Supervisor timeout","Power readings lost",
        "Finding running minimum","Restarting for floor check","Verifying running minimum","Stopping for direct start check","Verifying startup boost"};
    const int group=s.at("calibrationGroup");
    Elements rows{text("Calibration: "+(phase>=0 && phase<static_cast<int>(phases.size()) && (phase<6 || hardened)?phases[phase]:"Unknown / unverified")+
        (phase?" | Group "+std::to_string(group+1):""))|bold|color(phase>=7?Color::Red:active?Color::Yellow:Color::Cyan)};
    if(hardened && !active && phase>=5 && phase!=6)
        rows.push_back(paragraph("Previous calibration preserved. New measurements were NOT saved.")|color(Color::Yellow));
    if(!hardened) rows.push_back(paragraph("Legacy firmware: abort can lose RAM calibration; storage success is unverified. Upgrade to 1.4.0.")|color(Color::Yellow));
    if(active && group>=0 && group<2) {
        const auto& g=s.at("groups").at(group);
        rows.push_back(text("Sweep samples: "+s.at("calibrationStep").dump()+"/9 | Requested PWM: "+s.at("calibrationDutyPercent").dump()+"%"));
        rows.push_back(text("Actual RPM: "+g.at("fanRpm")[0].dump()+" / "+g.at("fanRpm")[1].dump()));
        rows.push_back(paragraph(s.value("calibratedMinimumSupported",false)?"Nano supervisor: repeated startup/floor tests, then 9 adaptive RPM points. 8s + 3 stable samples per sweep point; 18s phase / 12min run limits. Closing does not abort.":hardened?"Nano supervisor: 8s settle + 3 stable samples per sweep point; 18s point / 5min run limits. Closing the utility does not abort.":
            "Nano owns this routine. Closing or switching tabs does not abort it. Abort explicitly if needed.")|color(Color::Yellow));
    }
    rows.push_back(paragraph("Progress refreshes on this tab; temperature forwarding continues independently.")|dim);
    return vbox(rows);
}
}
