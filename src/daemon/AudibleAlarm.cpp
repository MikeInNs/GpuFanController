#include "AudibleAlarm.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

namespace fan {
AudibleAlarm::AudibleAlarm(Sound sound,std::chrono::milliseconds interval)
    :sound_(std::move(sound)),interval_(interval),worker_([this]{run();}) {}
AudibleAlarm::~AudibleAlarm() {
    {std::lock_guard lock(mutex_);stopping_=true;wake_.notify_all();}
    worker_.join();
}
void AudibleAlarm::enable(bool enabled) {
    std::lock_guard lock(mutex_);
    if(enabled_==enabled) return;
    enabled_=enabled;pending_=enabled && active_;
    state_["state"]=enabled?"not tested":"disabled";state_["error"]="";
    wake_.notify_all();
}
void AudibleAlarm::update(bool active,bool raised) {
    std::lock_guard lock(mutex_);
    const bool changed=active_!=active || (enabled_ && raised && !pending_);
    active_=active;
    if(enabled_ && raised) pending_=true; // A brief critical fault still gets one notification.
    if(changed) wake_.notify_all();
}
nlohmann::json AudibleAlarm::status() const {
    std::lock_guard lock(mutex_);auto result=state_;result["enabled"]=enabled_;
    result["criticalActive"]=active_;result["repeatSeconds"]=interval_.count()/1000;
    return result;
}
void AudibleAlarm::run() {
    auto next=std::chrono::steady_clock::time_point{};
    std::unique_lock lock(mutex_);
    while(!stopping_) {
        if(!enabled_ || (!active_ && !pending_)) {wake_.wait(lock);continue;}
        if(std::chrono::steady_clock::now()<next) {wake_.wait_until(lock,next);continue;}
        pending_=false;next=std::chrono::steady_clock::now()+interval_;
        state_["attempts"]=state_["attempts"].get<unsigned>()+1;
        lock.unlock();
        std::string error;
        try {sound_();} catch(const std::exception& e) {error=e.what();}
        lock.lock();
        if(!error.empty() && state_["error"]!=error) std::cerr<<"Motherboard beeper: "<<error<<'\n';
        if(enabled_) {
            state_["state"]=error.empty()?"device accepted tone":"unavailable";
            state_["error"]=error;
        }
        if(error.empty()) state_["lastSuccessAtMs"]=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}
void AudibleAlarm::pcSpeaker() {
    // Dedicated evdev speaker access, not /dev/console privileges or the broad input group.
    const int fd=open("/dev/gpu-fan-controller-speaker",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) throw std::runtime_error(std::string("Cannot open PC speaker: ")+std::strerror(errno));
    struct Close {int fd;~Close(){close(fd);}} closeDevice{fd};
    unsigned long sounds=0;
    if(ioctl(fd,EVIOCGBIT(EV_SND,sizeof(sounds)),&sounds)<0 || !(sounds&(1UL<<SND_TONE)))
        throw std::runtime_error("PC speaker device does not support SND_TONE");
    auto tone=[&](int frequency) {
        input_event event{};event.type=EV_SND;event.code=SND_TONE;event.value=frequency;
        ssize_t result;
        do {result=write(fd,&event,sizeof(event));} while(result<0 && errno==EINTR);
        return result==sizeof(event);
    };
    if(!tone(1800)) throw std::runtime_error("Cannot start PC speaker tone");
    // Finite pulse, independent of all forwarding and API locks. Normal shutdown joins this worker.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if(!tone(0)) {tone(0);throw std::runtime_error("Cannot stop PC speaker tone");}
}
}
