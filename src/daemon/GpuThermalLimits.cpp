#include "GpuThermalLimits.hpp"
#include "AmdGpu.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <regex>
#include <set>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace fan {
namespace {
std::string trim(const std::string& s) {
    const auto a=s.find_first_not_of(" \r\n\t");
    return a==std::string::npos?"":s.substr(a,s.find_last_not_of(" \r\n\t")-a+1);
}
void validPci(const std::string& pci) {
    if(!std::regex_match(pci,std::regex("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]")))
        throw std::invalid_argument("Invalid GPU PCI address");
}
std::string query(const std::string& pci) {
    int pipes[2];
    if(pipe2(pipes,O_CLOEXEC|O_NONBLOCK)) throw std::runtime_error("Cannot create thermal query pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,pipes[1],STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    // Fixed arguments and validated PCI; no shell, no GPU configuration writes.
    char exe[]="nvidia-smi", q[]="-q", d[]="-d", temperature[]="TEMPERATURE", i[]="-i";
    char* args[]{exe,q,d,temperature,i,const_cast<char*>(pci.c_str()),nullptr};
    pid_t pid;
    const int error=posix_spawnp(&pid,exe,&actions,nullptr,args,environ);
    posix_spawn_file_actions_destroy(&actions);close(pipes[1]);
    if(error) {close(pipes[0]);throw std::runtime_error("nvidia-smi unavailable; thermal limits cannot be read");}
    std::string output;bool exited=false,complete=false;int status=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<deadline) {
        char bytes[4096];const auto n=::read(pipes[0],bytes,sizeof(bytes));
        if(n>0) output.append(bytes,static_cast<std::size_t>(n));
        if(output.size()>65536) break;
        if(!exited) exited=waitpid(pid,&status,WNOHANG)==pid;
        if(exited && n==0) {complete=true;break;}
        pollfd p{pipes[0],POLLIN,0};poll(&p,1,20);
    }
    close(pipes[0]);
    if(!exited) {kill(pid,SIGKILL);waitpid(pid,&status,0);}
    if(!complete) throw std::runtime_error("Thermal query timed out or exceeded output limit");
    if(!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("nvidia-smi thermal query failed");
    return output;
}
}
nlohmann::json GpuThermalLimits::parseNvidia(const std::string& pci,const std::string& output) {
    validPci(pci);
    if(output.size()>65536) throw std::invalid_argument("Thermal report too large");
    using Json=nlohmann::json;
    Json values{{"targetDeciC",nullptr},{"operatingDeciC",nullptr},{"slowdownDeciC",nullptr},
                {"criticalDeciC",nullptr},{"shutdownDeciC",nullptr}};
    const std::map<std::string,std::string> fields{{"GPU Target Temperature","targetDeciC"},
        {"GPU Target Temperature Specification","targetDeciC"},{"GPU Max Operating Temp","operatingDeciC"},
        {"GPU Slowdown Temp","slowdownDeciC"},{"GPU Shutdown Temp","shutdownDeciC"}};
    std::set<std::string> seen;std::istringstream lines(output);std::string line;
    unsigned devices=0;bool temperature=false,sawTemperature=false;
    while(std::getline(lines,line)) {
        if(line.starts_with("GPU ")) {
            auto found=trim(line.substr(4));
            std::transform(found.begin(),found.end(),found.begin(),[](unsigned char c){return std::tolower(c);});
            if(found.size()==16 && found.starts_with("0000")) found.erase(0,4);
            if(++devices!=1 || found!=pci) throw std::invalid_argument("Thermal report GPU identity mismatch");
        }
        const auto clean=trim(line);
        if(clean=="Temperature") {temperature=devices==1;sawTemperature|=temperature;continue;}
        if(!temperature || clean.empty()) continue;
        // Only the selected GPU's Temperature subsection; ignore memory and T.Limit.
        if(line.find_first_not_of(' ')<8) {temperature=false;continue;}
        const auto colon=clean.find(':');if(colon==std::string::npos) continue;
        const auto key=trim(clean.substr(0,colon));const auto field=fields.find(key);
        if(field==fields.end()) continue;
        if(!seen.insert(field->second).second) throw std::invalid_argument("Duplicate thermal limit");
        auto value=trim(clean.substr(colon+1));if(value=="N/A") continue;
        if(!value.ends_with(" C")) throw std::invalid_argument("Expected absolute Celsius thermal limit");
        value=trim(value.substr(0,value.size()-2));int c=0;
        const auto [end,err]=std::from_chars(value.data(),value.data()+value.size(),c);
        if(err!=std::errc{} || end!=value.data()+value.size() || c<=0 || c>200)
            throw std::invalid_argument("Malformed thermal limit");
        values[field->second]=c*10;
    }
    if(devices!=1 || !sawTemperature) throw std::invalid_argument("Missing GPU thermal report");
    return {{"pciAddress",pci},{"vendor","NVIDIA"},{"sensor","gpu-core"},
        {"source","nvidia-smi GPU core temperature"},{"experimental",false},{"limits",values}};
}
nlohmann::json GpuThermalLimits::read(const std::string& pci) const {
    validPci(pci);const AmdGpu amd;
    if(amd.owns(pci)) return amd.thermalLimits(pci);
    // NVIDIA identifies the exact selected PCI in its response; unknown vendors fail closed.
    return parseNvidia(pci,query(pci));
}
}
