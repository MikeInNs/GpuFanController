#include "WakeSignal.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
using namespace std::chrono_literals;
using fan::WakeSignal;
void check(bool value) {if(!value) throw std::runtime_error("Wake signal assertion failed");}
int main() try {
    WakeSignal wake;
    // A notification before wait must not be lost, and multiple notifications
    // coalesce. Clearing restores a real, non-spinning deadline wait.
    wake.notify();wake.notify();
    auto start=WakeSignal::Clock::now();
    wake.wait({},start+1s);
    check(WakeSignal::Clock::now()-start<500ms);
    wake.clear();start=WakeSignal::Clock::now();
    wake.wait({},start+80ms);
    check(WakeSignal::Clock::now()-start>=80ms);
    // Both notification and stop interrupt an otherwise indefinite wait.
    for(bool cancel:{false,true}) {
        wake.clear();
        std::promise<void> complete;auto result=complete.get_future();
        std::jthread worker([&](std::stop_token stop) {
            wake.wait(stop,WakeSignal::Clock::time_point::max());complete.set_value();
        });
        std::this_thread::sleep_for(20ms);
        if(cancel) worker.request_stop();else wake.notify();
        check(result.wait_for(500ms)==std::future_status::ready);
    }
    wake.clear();std::stop_source stopped;stopped.request_stop();
    wake.wait(stopped.get_token(),WakeSignal::Clock::time_point::max());
    wake.clear();
    WakeSignal activity;
    activity.notify();
    start=WakeSignal::Clock::now();
    wake.wait({},start+1s,activity.descriptor());
    check(WakeSignal::Clock::now()-start<500ms);
    activity.clear();
    int sockets[2];check(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)==0);
    const char byte='a';check(write(sockets[1],&byte,1)==1);
    start=WakeSignal::Clock::now();
    wake.wait({},start+1s,activity.descriptor(),sockets[0]);
    check(WakeSignal::Clock::now()-start<500ms);
    char received;check(read(sockets[0],&received,1)==1);
    close(sockets[1]); // Disconnect must also wake, not wait for a heartbeat.
    start=WakeSignal::Clock::now();wake.wait({},start+1s,-1,sockets[0]);
    check(WakeSignal::Clock::now()-start<500ms);close(sockets[0]);
    std::cout<<"Deadline, coalesced notification, stop, serial readiness/disconnect passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
