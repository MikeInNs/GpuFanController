#pragma once
#include "ControllerData.hpp"
#include <string>

namespace fan::thermalSuggestion {
using Json=nlohmann::json;
struct Margins {int full=15,warning=10,critical=5,target=10;}; // Whole Celsius degrees.
struct Proposal {Json changes;std::string review;};
// Pure authoring policy: no I/O, no persistence, no invented RPM.
Proposal build(const Json& metadata,const Json& configuration,const Json& calibration,
               int group,const Json& desired,const Margins& margins,bool includeCurve);
}
