#include "Discovery.hpp"
#include "AmdGpu.hpp"
#include "SerialProbe.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
extern char** environ;
namespace fan {
namespace {
namespace fs = std::filesystem;
std::string readText(const fs::path& p) { std::ifstream f(p); std::string s; std::getline(f,s); return s; }
bool candidate(const std::string& path) {
    const auto name = fs::path(path).filename().string();
    if (path != "/dev/" + name || !std::regex_match(name,std::regex("tty(USB|ACM)[0-9]+"))) return false;
    std::error_code error;
    auto parent = fs::canonical(fs::path("/sys/class/tty")/name/"device",error);
    while (!error && parent != parent.root_path()) {
        const auto vendor = readText(parent/"idVendor");
        const auto product = readText(parent/"idProduct");
        if ((vendor == "1a86" && product == "7523") || vendor == "2341" || vendor == "2a03" ||
            (vendor == "0403" && product == "6001") || (vendor == "10c4" && product == "ea60")) return true;
        parent = parent.parent_path();
    }
    return false;
}
std::string gpuQuery() {
    int pipes[2];
    if (pipe2(pipes,O_CLOEXEC|O_NONBLOCK)) throw std::runtime_error("Cannot create GPU query pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,pipes[1],STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    char executable[] = "nvidia-smi";
    char query[] = "--query-gpu=uuid,pci.bus_id,name";
    char format[] = "--format=csv,noheader";
    char* args[]{executable,query,format,nullptr};
    pid_t pid;
    const int error = posix_spawnp(&pid,executable,&actions,nullptr,args,environ);
    posix_spawn_file_actions_destroy(&actions); close(pipes[1]);
    if (error) { close(pipes[0]); throw std::runtime_error("nvidia-smi unavailable; NVIDIA GPU discovery requires driver support"); }
    std::string output;
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    int status = 0; bool exited = false;
    while (std::chrono::steady_clock::now() < until) {
        char bytes[4096];
        auto count = read(pipes[0],bytes,sizeof(bytes));
        if (count > 0) output.append(bytes,static_cast<std::size_t>(count));
        if (output.size() > 65536) break;
        if (!exited) exited = waitpid(pid,&status,WNOHANG) == pid;
        if (exited && count <= 0) break;
        pollfd p{pipes[0],POLLIN,0}; poll(&p,1,20);
    }
    close(pipes[0]);
    if (!exited) { kill(pid,SIGKILL); waitpid(pid,&status,0); throw std::runtime_error("GPU query timed out"); }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("nvidia-smi GPU query failed");
    return output;
}
std::string trim(std::string s) { const auto start=s.find_first_not_of(" \r\n\t"); return start==std::string::npos ? "" : s.substr(start,s.find_last_not_of(" \r\n\t")-start+1); }
}
nlohmann::json Discovery::scan(std::stop_token stop) {
    auto result = nlohmann::json{{"controllers",nlohmann::json::array()}, {"gpus",nlohmann::json::array()}, {"errors",nlohmann::json::array()}};
    bool queryNvidia=true;
    try {
        const AmdGpu amd;
        auto devices=amd.discover();
        // Keep NVIDIA discovery as a fallback when PCI inventory is empty (WSL).
        queryNvidia=devices.empty() || amd.hasNvidia();
        for(auto& device:devices) {
            if(!device["temperatureError"].get<std::string>().empty()) result["errors"].push_back(device["temperatureError"]);
            result["gpus"].push_back(std::move(device));
        }
    } catch(const std::exception& e) {result["errors"].push_back(std::string("AMD discovery: ")+e.what());}
    if(queryNvidia)
    try {
        std::istringstream lines(gpuQuery()); std::string line;
        while (std::getline(lines,line)) {
            const auto a=line.find(','); const auto b=line.find(',',a==std::string::npos?0:a+1);
            if(a==std::string::npos || b==std::string::npos) throw std::runtime_error("Unexpected GPU query format");
            auto pci=trim(line.substr(a+1,b-a-1));
            std::transform(pci.begin(),pci.end(),pci.begin(),[](unsigned char c){return std::tolower(c);});
            if(pci.size()==16 && pci.substr(0,4)=="0000") pci.erase(0,4);
            if (!std::regex_match(pci,std::regex("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]"))) throw std::runtime_error("Invalid GPU PCI address");
            result["gpus"].push_back({{"uuid",trim(line.substr(0,a))},{"pciAddress",pci},{"name",trim(line.substr(b+1))},{"vendor","NVIDIA"}});
        }
    } catch(const std::exception& e) {result["errors"].push_back(e.what());}
    std::vector<std::string> paths;
    for (const auto& device : fs::directory_iterator("/dev")) if(candidate(device.path().string())) paths.push_back(device.path().string());
    std::sort(paths.begin(),paths.end());
    if(paths.size()>8) { result["errors"].push_back("Scan limited to 8 candidate serial devices"); paths.resize(8); }
    std::erase_if(probes_,[&](const auto& entry){return std::find(paths.begin(),paths.end(),entry.first)==paths.end();});
    for(const auto& path:paths) {
        if(stop.stop_requested()) break;
        try {
            if(!probes_.contains(path)) probes_[path]=std::make_shared<SerialProbe>(path);
            auto c=connected(path).hello(); c["path"]=path; result["controllers"].push_back(c);
        }
        catch(const std::exception& e) { probes_.erase(path); result["errors"].push_back(path+": "+e.what()); }
    }
    return result;
}
nlohmann::json Discovery::claim(const std::string& path) {
    if(!candidate(path)) throw std::invalid_argument("Not an eligible USB serial device");
    auto& probe=connected(path); auto controller=probe.hello();
    if(controller["controllerId"].is_null()) {
        auto id=readText("/proc/sys/kernel/random/uuid");
        id.erase(std::remove(id.begin(),id.end(),'-'),id.end());
        if(!std::regex_match(id,std::regex("[0-9a-f]{32}"))) throw std::runtime_error("Cannot generate identity");
        probe.setIdentity(id); controller=probe.hello();
    }
    controller["path"]=path; return controller;
}
SerialProbe& Discovery::connected(const std::string& path) {
    auto found=probes_.find(path);
    if(found==probes_.end()) throw std::invalid_argument("Controller disconnected; scan again");
    return *found->second;
}
}
