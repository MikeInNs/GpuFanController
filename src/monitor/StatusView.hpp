#pragma once
#include "MonitorModel.hpp"
#include <ftxui/dom/elements.hpp>
namespace fan {
ftxui::Element statusView(const Json& snapshot, int group, bool live=false);
ftxui::Element calibrationView(const Json& snapshot, int group);
}
