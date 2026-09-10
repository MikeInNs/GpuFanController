#include "GpuTemperatures.hpp"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void check(bool value) {if(!value) throw std::runtime_error("Temperature assertion failed");}
template<class F> void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
int main() {
    using namespace fan;
    const auto now=SteadyClock::time_point{}+100s;
    auto readings=NvidiaTemperatures::parse("00000000:01:00.0, 49.04\n00000000:02:00.0, 61.25\n",now);
    check(readings.temperatures.at("0000:01:00.0")==490);
    check(readings.temperatures.at("0000:02:00.0")==613);
    nlohmann::json groups=nlohmann::json::array({{{"enabled",true},{"gpuPciAddress","0000:02:00.0"}},{{"enabled",false},{"gpuPciAddress",nullptr}}});
    auto s=mappedTemperatures(groups,readings,now+1s);
    check(s.valid_mask==1 && s.group1_deci_celsius==613 && s.group2_deci_celsius==0);
    check(mappedTemperatures(groups,readings,now+3s).valid_mask==0);
    groups[1]={{"enabled",true},{"gpuPciAddress","0000:01:00.0"}};
    check(mappedTemperatures(groups,readings,now).valid_mask==3);
    auto invalid=NvidiaTemperatures::parse("00000000:01:00.0, N/A\n00000000:02:00.0, 61\n",now);
    check(mappedTemperatures(groups,invalid,now).valid_mask==1);
    for(const auto* bad:{"nan","inf","125.1","-40.1","49garbage","[Not Supported]"}) {
        check(NvidiaTemperatures::parse(std::string("0000:01:00.0, ")+bad,now).temperatures.empty());
    }
    check(NvidiaTemperatures::parse("",now).temperatures.empty());
    rejects([&]{NvidiaTemperatures::parse("notpci, 50",now);});
    rejects([&]{NvidiaTemperatures::parse("0000:01:00.0, 50\n0000:01:00.0, 51",now);});
    TemperatureSchedule schedule;
    check(schedule.due(s,now));schedule.sent(s,now);
    check(schedule.nextDue(s,now)==now+5s);
    check(!schedule.due(s,now+4999ms));check(schedule.due(s,now+5s));
    auto changed=s;++changed.group1_deci_celsius;
    check(schedule.nextDue(changed,now)==now+1s);
    check(!schedule.due(changed,now+999ms));check(schedule.due(changed,now+1s));
    schedule.sent(changed,now+1s);
    check(schedule.nextDue(changed,now+1s)==now+6s);
    check(!schedule.due(changed,now+5999ms));check(schedule.due(changed,now+6s));
    auto missing=changed;missing.valid_mask=0;missing.group1_deci_celsius=0;
    check(schedule.nextDue(missing,now+1s)==now+2s);
    check(!schedule.due(missing,now+1999ms));check(schedule.due(missing,now+2s));
    schedule.reset();check(schedule.due(s,now+2s));
    std::cout<<"PCI mapping, invalid/stale readings, quantization, 1s/5s scheduling passed\n";
}
