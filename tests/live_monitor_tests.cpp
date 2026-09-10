#include "LiveMonitor.hpp"
#include <atomic>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
using fan::LiveMonitor;
using Json=nlohmann::json;
void require(bool ok) {if(!ok) throw std::runtime_error("Live monitor assertion failed");}
int main() try {
    std::atomic<int> calls=0,active=0,peak=0;
    std::atomic<bool> release=false,fail=false;
    LiveMonitor monitor([&](const std::string& id) {
        ++calls;const int running=++active;
        int old=peak;while(old<running && !peak.compare_exchange_weak(old,running)) {}
        if(id=="a") while(!release) std::this_thread::sleep_for(5ms);
        --active;
        if(fail) throw std::runtime_error("offline");
        return Json{{"controllerId",id},{"status",{{"groups",Json::array({Json::object(),Json::object()})}}}};
    });
    const std::vector<std::string> ids{"a","b","c"};
    auto pump=[&](auto duration,bool launch) {
        const auto until=LiveMonitor::Clock::now()+duration;
        do {monitor.tick(ids,launch);std::this_thread::sleep_for(10ms);} while(LiveMonitor::Clock::now()<until);
    };
    pump(150ms,true);
    const bool independent=monitor.sample("a")->busy &&
        monitor.sample("b")->fresh(LiveMonitor::Clock::now()) &&
        monitor.sample("c")->fresh(LiveMonitor::Clock::now()) && calls==3 && peak<=2;
    release=true;pump(100ms,false);
    require(independent); // Slow A does not block healthy B/C.
    require(monitor.sample("a")->fresh(LiveMonitor::Clock::now()));
    pump(1100ms,false);require(calls==3); // Hidden/modal UI never launches.
    fail=true;pump(100ms,true);
    require(!monitor.sample("b")->fresh(LiveMonitor::Clock::now()));
    require(monitor.sample("b")->error=="offline");
    const auto count=calls.load();pump(200ms,true);require(calls==count);
    fail=false;pump(1100ms,true);
    require(monitor.sample("b")->fresh(LiveMonitor::Clock::now()));
    auto sample=*monitor.sample("b");
    require(!sample.fresh(sample.received+5s));
    monitor.tick({},false);require(!monitor.sample("a") && !monitor.sample("b"));
    LiveMonitor wrong([](const auto&) {return Json{{"controllerId","wrong"}};});
    wrong.tick({"a"},true);std::this_thread::sleep_for(50ms);wrong.tick({"a"},false);
    require(!wrong.sample("a")->error.empty() && wrong.sample("a")->status.is_null());
    std::cout<<"Bounded independent reads, identity checks, stale/failure hiding, pacing and stop passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
