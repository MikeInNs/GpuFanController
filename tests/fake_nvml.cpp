#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
struct Device {unsigned index;};
Device devices[2]{{1},{2}};
bool initialized=false;
std::string mode() {std::ifstream input(std::getenv("FAN_TEST_NVML_STATE"));std::string result;input>>result;return result;}
void log(const std::string& event) {std::ofstream output(std::getenv("FAN_TEST_NVML_LOG"),std::ios::app);output<<getpid()<<' '<<event<<'\n';}
}
extern "C" {
int nvmlInit_v2() {log("init");if(mode()=="init-fail") return 999;initialized=true;return 0;}
int nvmlShutdown() {log("shutdown");initialized=false;if(mode()=="shutdown-hang") sleep(10);return 0;}
int nvmlDeviceGetHandleByPciBusId_v2(const char* pci,Device** device) {
    log(std::string("handle ")+pci);
    if(!initialized) return 1;
    if(mode()=="handle-fail") return 6;
    const std::string address=pci;
    if(address=="0000:01:00.0") *device=&devices[0];
    else if(address=="0000:02:00.0") *device=&devices[1];
    else return 6;
    return 0;
}
#ifndef FAN_TEST_MISSING_SYMBOL
int nvmlDeviceGetTemperature(Device* device,int sensor,unsigned* value) {
    log("temperature "+std::to_string(device->index));
    if(sensor!=0) return 2;
    const auto current=mode();
    if(current=="hang") sleep(10);
    if(current=="crash") _exit(7);
    if(current=="uninitialized") {initialized=false;return 1;}
    if(!initialized) return 1;
    if(device->index==2) {
        if(current=="lost") return 15;
        if(current=="unknown") return 999;
        if(current=="unsupported") return 3;
        if(current=="invalid-handle") return 2;
    }
    *value=current=="range"?126:current=="zero"?0:40+device->index;
    return 0;
}
#endif
const char* nvmlErrorString(int code) {switch(code){case 1:return "Uninitialized";case 3:return "Not Supported";case 6:return "Not Found";case 15:return "GPU is lost";default:return "Unknown Error";}}
}
