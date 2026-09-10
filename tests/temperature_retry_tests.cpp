#include "TemperatureRetry.hpp"
#include "TemperatureDiagnostics.hpp"
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;
void check(bool value) {if(!value) throw std::runtime_error("Temperature retry assertion failed");}
int main() {
    using namespace fan;using Json=nlohmann::json;
    const auto now=SteadyClock::time_point{}+100s;
    const auto groups=Json::array({{{"enabled",true},{"gpuPciAddress","0000:01:00.0"}},{{"enabled",true},{"gpuPciAddress","0000:02:00.0"}}});
    auto sample=[&](SteadyClock::time_point time) {return GpuReadings{{{"0000:01:00.0",500},{"0000:02:00.0",600}},time,{}};};
    auto failure=[&](SteadyClock::time_point time) {return GpuReadings{{},time,"NVML unavailable"};};
    TemperatureRetry retry;TemperatureDiagnostics diagnostics;
    retry.begin();check(!retry.accept(sample(now)));
    retry.begin();auto failed=failure(now+1s);
    check(retry.accept(failed));
    diagnostics.queryCompleted(0,failed,{now+1s,10ms,false,1,true},now+1s+10ms);
    check(mappedTemperatures(groups,retry.readings(),now+1s).valid_mask==3);
    check(retry.readings().timestampFor("0000:01:00.0")==now);
    check(retry.accept(failure(now+1250ms)));
    diagnostics.queryCompleted(0,failed,{now+1250ms,10ms,false,2,true},now+1260ms);
    check(retry.readings().timestampFor("0000:02:00.0")==now);
    check(!retry.accept(sample(now+1500ms)));
    diagnostics.queryCompleted(0,sample(now+1500ms),{now+1500ms,10ms,false,3,false},now+1510ms);
    check(diagnostics.history().empty()); // Failed attempts and successful retry are silent.
    check(mappedTemperatures(groups,retry.readings(),now+3s).valid_mask==3);

    // Three failures invalidate immediately, without waiting for sample expiry.
    retry.begin();check(retry.accept(failure(now+2s)));check(retry.accept(failure(now+2250ms)));
    check(!retry.accept(failure(now+2500ms)));
    diagnostics.queryCompleted(0,failed,{now+2500ms,10ms,false,3,false},now+2510ms);
    check(mappedTemperatures(groups,retry.readings(),now+2500ms).valid_mask==0);
    check(diagnostics.history().size()==1 && diagnostics.history().back()["attempt"]==3);
    // An exhausted reading cannot be resurrected by the next cycle's retries.
    retry.begin();check(retry.accept(failure(now+3s)));
    check(mappedTemperatures(groups,retry.readings(),now+3s).valid_mask==0);
    check(retry.accept(failure(now+3250ms)));check(!retry.accept(failure(now+3500ms)));
    diagnostics.queryCompleted(0,failed,{now+3500ms,10ms,false,3,false},now+3510ms);
    check(diagnostics.history().size()==1); // Repeated exhausted cycles do not spam.
    retry.begin();check(!retry.accept(sample(now+4s)));
    diagnostics.queryCompleted(0,sample(now+4s),{now+4s,10ms,false,1,false},now+4010ms);
    check(diagnostics.history().size()==2 && diagnostics.history().back()["transition"]=="recovered");

    // A partial query publishes a new high temperature immediately without
    // refreshing the retained channel's age; each GPU expires independently.
    retry.begin();GpuReadings partial{{{"0000:01:00.0",950}},now+5s,"GPU 2 unavailable"};
    check(retry.accept(partial));
    auto mapped=mappedTemperatures(groups,retry.readings(),now+5s);
    check(mapped.valid_mask==3 && mapped.group1_deci_celsius==950 && mapped.group2_deci_celsius==600);
    check(retry.readings().timestampFor("0000:01:00.0")==now+5s);
    check(retry.readings().timestampFor("0000:02:00.0")==now+4s);
    check(mappedTemperatures(groups,retry.readings(),now+7s).valid_mask==1);
    // The last cached reading cannot outlive the three-second deadline even
    // if the next query is still running and no retry has completed yet.
    check(mappedTemperatures(groups,retry.readings(),now+8s).valid_mask==0);
    partial.sampledAt=now+8s;check(retry.accept(partial));
    partial.sampledAt=now+8250ms;check(!retry.accept(partial));
    check(mappedTemperatures(groups,retry.readings(),now+8250ms).valid_mask==1);
    check(!retry.readings().temperatures.contains("0000:02:00.0"));

    // No value is invented on startup or after all mappings have been removed.
    retry.clear();retry.begin();check(retry.accept(failure(now+9s)));
    check(mappedTemperatures(groups,retry.readings(),now+9s).valid_mask==0);
    retry.clear();check(retry.readings().temperatures.empty());
    check(TemperatureRetry::maxAttempts==3 && TemperatureRetry::delay==250ms);
    std::cout<<"Three attempts, retained original timestamps, per-GPU partial retries, expiry, exhaustion and quiet logging passed\n";
}
