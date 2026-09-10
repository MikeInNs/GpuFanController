#pragma once
#include "LiveMonitor.hpp"
#include <ftxui/dom/elements.hpp>
namespace fan {
ftxui::Element liveStatusView(const LiveMonitor::Sample* sample,int group);
ftxui::Element overviewView(const nlohmann::json& controllers,const LiveMonitor& monitor,bool available);
}
