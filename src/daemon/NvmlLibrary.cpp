#include "NvmlLibrary.hpp"
#include <dlfcn.h>
#include <stdexcept>

namespace fan {
namespace {
template<class T> T symbol(void* library,const char* name) {
    auto pointer=dlsym(library,name);
    if(!pointer) throw std::runtime_error(std::string("NVML missing symbol: ")+name);
    return reinterpret_cast<T>(pointer);
}
}
NvmlLibrary::~NvmlLibrary() {
    if(initialized_) shutdown_();
    if(library_) dlclose(library_);
}
std::string NvmlLibrary::failure(const std::string& operation,int code) const {
    const char* detail=error_?error_(code):nullptr;
    return "NVML "+operation+" failed (code "+std::to_string(code)+"): "+
        (detail?std::string(detail).substr(0,256):"unknown error");
}
void NvmlLibrary::initialize() {
    if(!library_) {
        library_=dlopen("libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL);
        if(!library_) library_=dlopen("/usr/lib/wsl/lib/libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL);
        if(!library_) throw std::runtime_error("NVML library unavailable: install/check the NVIDIA driver (libnvidia-ml.so.1)");
    }
    // Resolve once, and resolve every required function before calling any.
    if(!init_ || !shutdown_ || !handle_ || !temperature_ || !error_) {
        init_=symbol<decltype(init_)>(library_,"nvmlInit_v2");
        shutdown_=symbol<decltype(shutdown_)>(library_,"nvmlShutdown");
        handle_=symbol<decltype(handle_)>(library_,"nvmlDeviceGetHandleByPciBusId_v2");
        temperature_=symbol<decltype(temperature_)>(library_,"nvmlDeviceGetTemperature");
        error_=symbol<decltype(error_)>(library_,"nvmlErrorString");
    }
    if(reinitialize_) {
        if(initialized_) {
            const auto code=shutdown_();
            if(code && code!=1) throw std::runtime_error(failure("shutdown for recovery",code));
        }
        initialized_=false;reinitialize_=false;handles_.clear();failures_.clear();
    }
    if(!initialized_) {
        const auto code=init_();
        if(code) throw std::runtime_error(failure("initialization",code));
        initialized_=true;
    }
}
nlohmann::json NvmlLibrary::sample(const std::set<std::string>& addresses) {
    auto result=nlohmann::json{{"temperatures",nlohmann::json::object()},{"error",""}};
    if(addresses.empty()) return result;
    try {initialize();} catch(const std::exception& e) {result["error"]=e.what();return result;}
    std::erase_if(handles_,[&](const auto& entry){return !addresses.contains(entry.first);});
    std::erase_if(failures_,[&](const auto& entry){return !addresses.contains(entry.first);});
    std::string errors;
    for(const auto& pci:addresses) {
        Handle device=nullptr;
        int code=0;
        if(const auto found=handles_.find(pci);found!=handles_.end()) device=found->second;
        else {
            code=handle_(pci.c_str(),&device);
            if(!code && device) handles_[pci]=device;
            else if(!code) code=999;
        }
        const bool obtainingHandle=!device;
        unsigned celsius=0;
        if(!code) code=temperature_(device,0,&celsius); // NVML_TEMPERATURE_GPU = 0
        if(!code && celsius<=125) {
            result["temperatures"][pci]=celsius*10;
            failures_.erase(pci);
        } else {
            if(!errors.empty()) errors+="; ";
            errors+=code?failure(pci+(obtainingHandle?" handle":" temperature"),code):"NVML "+pci+" temperature outside 0..125 C";
            // Transient read failures keep the session. Lost/invalid handles are
            // reacquired; global context failures and repeated unknown errors
            // reinitialize on the NEXT request, preserving this batch's successes.
            if(code==1 || code==9 || code==15 || code==16) {handles_.erase(pci);reinitialize_=true;}
            else if(code==2 || code==6) {handles_.erase(pci);failures_.erase(pci);}
            else if(code==999) {
                if(++failures_[pci]>=3) {handles_.erase(pci);reinitialize_=true;}
            } else failures_.erase(pci);
        }
    }
    result["error"]=errors.substr(0,4096);
    return result;
}
}
