#include "ThermalSuggestion.hpp"
#include <algorithm>
#include <optional>
#include <sstream>

namespace fan::thermalSuggestion {
namespace {
void require(bool ok,const char* message) {if(!ok) throw std::invalid_argument(message);}
std::optional<int> limit(const Json& limits,const char* key) {
    const auto& value=limits.at(key);if(value.is_null()) return {};
    require(value.is_number_integer(),"Malformed GPU limit (expected absolute tenths C)");
    const auto n=value.get<long long>();require(n>0 && n<=2000,"GPU limit outside plausible range");
    return static_cast<int>(n);
}
std::string degrees(int n) {std::ostringstream s;s<<n/10;if(n%10) s<<'.'<<n%10;return s.str()+" C";}
std::string shown(const std::optional<int>& n) {return n?degrees(*n):"unavailable";}
std::string point(const Json& p) {
    const auto& t=p.at("temperatureDeciC");
    return (t.is_number_integer()?degrees(t.get<int>()):t.dump()+" tenths C")+" / "+p.at("rpm").dump()+" RPM";
}
}
Proposal build(const Json& metadata,const Json& configuration,const Json& cal,int group,
               const Json& desired,const Margins& m,bool includeCurve) {
    require(group==0 || group==1,"Invalid fan group");
    const bool amd=metadata.at("vendor")=="AMD";
    require(amd || metadata.at("vendor")=="NVIDIA","Unsupported GPU vendor");
    require(metadata.at("sensor")== (amd?"gpu-edge":"gpu-core"),"GPU limit sensor does not match the control source");
    const auto& limits=metadata.at("limits");
    auto target=limit(limits,"targetDeciC"),operating=limit(limits,"operatingDeciC"),slowdown=limit(limits,"slowdownDeciC"),
         critical=limit(limits,"criticalDeciC"),shutdown=limit(limits,"shutdownDeciC");
    std::optional<int> reference;
    if(amd) {
        require(!target && !operating && !slowdown,"Unexpected AMD edge limit semantics");reference=critical;
    } else {
        require(!critical,"Unexpected NVIDIA critical limit");
        require(!operating || !slowdown || *operating<=*slowdown,"Inconsistent operating/slowdown limits");
        reference=operating?operating:slowdown;
        if(reference && slowdown) reference=std::min(*reference,*slowdown);
    }
    require(reference.has_value(),"No usable operating/slowdown or AMD edge critical limit. Configure manually; shutdown alone is insufficient.");
    require(!shutdown || *reference<=*shutdown,"Reference limit exceeds shutdown/emergency limit");
    require(!target || *target<=*reference,"GPU target exceeds the reference limit");
    for(int margin:{m.full,m.warning,m.critical,m.target}) require(margin>=1 && margin<=100,"Margins must be whole degrees C from 1 to 100");
    int full=*reference-m.full*10;const int warn=*reference-m.warning*10,crit=*reference-m.critical*10;
    if(target) full=std::min(full,*target-m.target*10);
    require(full>=0 && full<=1200 && warn>=0 && warn<=1199 && crit>=1 && crit<=1200,"Suggested temperatures exceed Nano limits; adjust margins (no clamping)");
    require(full<=warn && warn<crit && crit<*reference,"Require full-speed <= warning < critical < reference; adjust margins");
    require(!shutdown || crit<*shutdown,"Suggested critical must be below shutdown/emergency");
    Proposal result{{{"warningTemperatureDeciC",warn},{"criticalTemperatureDeciC",crit}}, {}};
    if(includeCurve) {
        const auto& base=configuration.at("groups").at(group);
        require(desired.at("expectedFanMask")==base.at("expectedFanMask"),"Save fan selection and recalibrate before curve assistance");
        require(desired.at("minimumDutyPercent")==base.at("minimumDutyPercent"),"Save minimum PWM changes before curve assistance");
        const auto range=controller::rpmRange(cal,base);
        require(!range.is_null(),"Usable calibration required for curve assistance; uncheck curve or calibrate first");
        auto curve=desired.at("curve");
        require(curve.is_array() && !curve.empty() && curve.size()<=4,"Curve requires 1 to 4 points");
        curve.back()={{"temperatureDeciC",full},{"rpm",range.at("maximum")}};
        result.changes["curve"]=curve;
    }
    // Reuse Nano-compatible range/order validation. Earlier curve points are never repaired silently.
    controller::editConfiguration(controller::configurationBytes(configuration),group,result.changes,cal);
    result.review=std::string(amd?"AMD experimental / GPU edge\n":"NVIDIA / GPU core\n")+
        "Target: "+shown(target)+"\nMax operating: "+shown(operating)+"\nSlowdown: "+shown(slowdown)+
        "\nReported AMD critical: "+shown(critical)+"\nShutdown/emergency: "+shown(shutdown)+
        "\nReference: "+degrees(*reference)+"\n\n";
    result.review+="Full-speed temperature: reference - "+std::to_string(m.full)+" C";
    if(target) result.review+=", capped at target - "+std::to_string(m.target)+" C";
    result.review+=" = "+degrees(full)+"\nWarning: reference - "+std::to_string(m.warning)+" C = "+degrees(warn)+
        " (was "+desired.at("warningTemperatureDeciC").dump()+" tenths C)\nCritical: reference - "+std::to_string(m.critical)+
        " C = "+degrees(crit)+" (was "+desired.at("criticalTemperatureDeciC").dump()+" tenths C)\n";
    if(includeCurve) {
        result.review+="Final curve point: "+point(desired.at("curve").back())+" -> "+point(result.changes.at("curve").back())+
            "\nOnly the final point changes; maximum RPM comes from calibration.\n";
    } else result.review+="Curve unchanged. The full-speed temperature above is informational until curve assistance is selected.\n";
    result.review+="\nThese are starting suggestions, NOT manufacturer-approved or guaranteed safe settings. Stop workloads and validate cooling under supervision. "
        "Accepting only updates this group's draft; Save changes is still required. No GPU settings are modified.";
    return result;
}
}
