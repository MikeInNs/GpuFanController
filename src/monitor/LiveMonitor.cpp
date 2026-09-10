#include "LiveMonitor.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace fan {
void LiveMonitor::tick(const std::vector<std::string>& ids,bool launch) {
    const auto now=Clock::now();
    for(auto it=jobs_.begin();it!=jobs_.end();) {
        if(it->future.wait_for(std::chrono::seconds(0))!=std::future_status::ready) {++it;continue;}
        auto result=it->future.get();auto& s=samples_[it->id];s.busy=false;
        s.error=result.error;s.next=now+std::chrono::seconds(1);
        if(result.error.empty()) {s.status=std::move(result.status);s.received=result.received;}
        it=jobs_.erase(it);
    }
    const std::set<std::string> wanted(ids.begin(),ids.end());
    for(auto it=samples_.begin();it!=samples_.end();) {
        if(!wanted.contains(it->first) && !it->second.busy) it=samples_.erase(it);else ++it;
    }
    if(!launch || ids.empty()) return;
    // Two bounded HTTP jobs: a slow board cannot freeze the UI or serialize all boards.
    for(std::size_t checked=0;checked<ids.size() && jobs_.size()<2;++checked) {
        const auto id=ids[cursor_++%ids.size()];auto& s=samples_[id];
        if(s.busy || now<s.next) continue;
        s.busy=true;
        jobs_.push_back({id,std::async(std::launch::async,[read=read_,id]{
            Result result;
            try {
                auto response=read(id);
                if(response.at("controllerId")!=id || !response.at("status").is_object() || response.at("status").at("groups").size()!=2)
                    throw std::runtime_error("Invalid live status identity/payload");
                result.status=response.at("status");
            } catch(const std::exception& e) {result.error=e.what();}
            result.received=Clock::now();return result;
        })});
    }
}
const LiveMonitor::Sample* LiveMonitor::sample(const std::string& id)const {
    auto it=samples_.find(id);return it==samples_.end()?nullptr:&it->second;
}
}
