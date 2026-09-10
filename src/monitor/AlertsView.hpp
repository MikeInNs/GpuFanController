#pragma once
#include <nlohmann/json.hpp>
#include <ftxui/dom/elements.hpp>
#include <ctime>

namespace fan {
inline ftxui::Element alertsView(const nlohmann::json& alerts,const std::string& error) {
    using namespace ftxui;
    Elements rows{text("Alerts - all configured controllers")|bold};
    if(!error.empty()) rows.push_back(paragraph("ALERT FEED STALE: "+error)|color(Color::Red));
    if(alerts.is_null()) {rows.push_back(text("Alert feed unavailable; update/reload the daemon."));return vbox(rows);}
    const auto& beeper=alerts.at("beeper");
    rows.push_back(paragraph(std::string("Motherboard beeper: ")+(beeper.at("enabled").get<bool>()?"ON - ":"OFF - ")+beeper.at("state").get<std::string>()));
    if(!beeper.at("error").get<std::string>().empty()) rows.push_back(paragraph(beeper.at("error").get<std::string>())|color(Color::Yellow));
    rows.push_back(paragraph("Critical faults: one 200ms beep, repeated at most every 30s. Disabling sound does not clear faults or change Nano protection.")|dim);
    if(alerts.at("controllers").empty()) rows.push_back(text("No controllers monitored (check mappings/forwarding).")|color(Color::Yellow));
    for(const auto& c:alerts.at("controllers")) if(!c.at("known").get<bool>() || c.at("stale").get<bool>())
        rows.push_back(paragraph(c.at("controllerId").get<std::string>()+": fault state unknown/stale")|color(Color::Yellow));
    rows.push_back(separator());
    rows.push_back(text("Active faults")|bold);
    auto label=[](const auto& e) {
        return e.at("controllerId").template get<std::string>().substr(0,8)+" / "+
            (e.at("group").is_null()?e.at("scope").template get<std::string>():"group "+std::to_string(e.at("group").template get<int>()+1))+": "+
            e.at("message").template get<std::string>();
    };
    if(alerts.at("active").empty()) rows.push_back(text("No reported active faults (not a hardware safety verdict)."));
    for(const auto& a:alerts.at("active")) rows.push_back(paragraph(label(a)+(a.at("stale").get<bool>()?" [last known]":""))|color(a.at("severity")=="critical"?Color::Red:Color::Yellow));
    rows.push_back(separator());rows.push_back(text("Recent transitions (newest first, daemon session only)")|bold);
    for(auto it=alerts.at("history").rbegin();it!=alerts.at("history").rend();++it) {
        const std::time_t seconds=it->at("receivedAtMs").template get<std::int64_t>()/1000;
        std::tm tm{};gmtime_r(&seconds,&tm);char timestamp[32]{};std::strftime(timestamp,sizeof(timestamp),"%m-%d %H:%M:%S UTC",&tm);
        rows.push_back(paragraph(std::string(timestamp)+" #"+it->at("sequence").dump()+" "+it->at("transition").template get<std::string>()+" "+label(*it))|
            color(it->at("transition")=="cleared"?Color::Green:it->at("severity")=="critical"?Color::Red:Color::Yellow));
    }
    if(alerts.at("historyTruncated").get<bool>()) rows.push_back(text("Older events evicted; inspect the daemon journal.")|dim);
    return vbox(rows);
}
}
