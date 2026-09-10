#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <sys/eventfd.h>
#include <unistd.h>

namespace fan {
// One waiting thread, any number of notifiers. Clear BEFORE inspecting the
// state that determines the next deadline, so concurrent notifications survive.
class WakeSignal {
public:
    using Clock=std::chrono::steady_clock;
    WakeSignal():fd_(eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK)) {
        if(fd_<0) throw std::runtime_error("Cannot create worker wake signal");
    }
    ~WakeSignal() {close(fd_);}
    WakeSignal(const WakeSignal&)=delete;
    WakeSignal& operator=(const WakeSignal&)=delete;
    int descriptor() const {return fd_;}
    void notify() const noexcept {
        const std::uint64_t value=1;
        while(write(fd_,&value,sizeof(value))<0 && errno==EINTR) {}
        // EAGAIN means a notification is already pending.
    }
    void clear() const noexcept {
        std::uint64_t value;
        while(read(fd_,&value,sizeof(value))<0 && errno==EINTR) {}
    }
    void wait(std::stop_token stop,Clock::time_point deadline,
              int activity=-1,int serial=-1) const {
        std::stop_callback cancel(stop,[this]{notify();});
        while(!stop.stop_requested()) {
            int timeout=-1;
            if(deadline!=Clock::time_point::max()) {
                const auto remaining=deadline-Clock::now();
                if(remaining<=Clock::duration::zero()) return;
                timeout=static_cast<int>(std::min<std::int64_t>(
                    std::chrono::ceil<std::chrono::milliseconds>(remaining).count(),
                    std::numeric_limits<int>::max()));
            }
            pollfd descriptors[]={{fd_,POLLIN,0},{activity,POLLIN,0},{serial,POLLIN,0}};
            const auto result=poll(descriptors,3,timeout);
            if(result<0 && errno==EINTR) continue;
            if(result<0) throw std::runtime_error("Worker wait failed");
            return;
        }
    }
private:
    int fd_;
};
}
