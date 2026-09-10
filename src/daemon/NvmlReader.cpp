#include "NvmlReader.hpp"
#include "NvmlHelper.hpp"
#include "AmdGpu.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <regex>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;

namespace fan {
NvmlReader::NvmlReader(std::string executable,std::chrono::milliseconds timeout)
    :executable_(std::move(executable)),timeout_(timeout) {}
NvmlReader::~NvmlReader() {stop();}
bool NvmlReader::reap() noexcept {
    if(pid_<0) return true;
    int status=0;
    const auto result=waitpid(pid_,&status,WNOHANG);
    if(result==pid_ || (result<0 && errno==ECHILD)) {pid_=-1;return true;}
    return false;
}
void NvmlReader::stop() noexcept {
    if(socket_>=0) {close(socket_);socket_=-1;}
    // Give normal shutdown a chance to release NVML; never wait indefinitely
    // for a hung driver. Keep an unreaped PID so retries cannot accumulate helpers.
    for(int i=0;i<10 && !reap();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(pid_>=0) kill(pid_,SIGKILL);
    for(int i=0;i<10 && !reap();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
void NvmlReader::start() {
    if(socket_>=0) return;
    if(!reap()) throw std::runtime_error("NVML helper is still terminating; restart deferred");
    int pair[2];
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)) throw std::runtime_error("Cannot create NVML helper socket");
    // Keep the source away from stdin/stdout/stderr/IPC target during spawn actions.
    const int child=fcntl(pair[1],F_DUPFD_CLOEXEC,4);close(pair[1]);
    if(child<0) {close(pair[0]);throw std::runtime_error("Cannot duplicate NVML helper socket");}
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int code=posix_spawn_file_actions_init(&actions);
    if(code) {close(pair[0]);close(child);throw std::runtime_error("Cannot initialize NVML spawn actions");}
    code=posix_spawnattr_init(&attributes);
    if(code) {posix_spawn_file_actions_destroy(&actions);close(pair[0]);close(child);throw std::runtime_error("Cannot initialize NVML spawn attributes");}
    auto add=[&](int result){if(!code) code=result;};
    add(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0));
    add(posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0));
    add(posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0));
    add(posix_spawn_file_actions_adddup2(&actions,child,nvmlHelperFd));
    add(posix_spawn_file_actions_addclosefrom_np(&actions,nvmlHelperFd+1));
    sigset_t empty;sigemptyset(&empty);
    add(posix_spawnattr_setsigmask(&attributes,&empty));
    add(posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK));
    char mode[]="--nvml-helper";
    char* argv[]{executable_.data(),mode,nullptr};
    if(!code) code=posix_spawn(&pid_,executable_.c_str(),&actions,&attributes,argv,environ);
    posix_spawnattr_destroy(&attributes);posix_spawn_file_actions_destroy(&actions);close(child);
    if(code) {close(pair[0]);pid_=-1;throw std::runtime_error(std::string("NVML helper spawn failed: ")+std::strerror(code));}
    socket_=pair[0];
}
GpuReadings NvmlReader::sample(const std::set<std::string>& addresses) {
    const auto started=SteadyClock::now();
    std::set<std::string> selected;
    const AmdGpu amd;
    static const std::regex pciPattern("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]");
    for(const auto& pci:addresses) {
        if(!std::regex_match(pci,pciPattern)) throw std::invalid_argument("Invalid NVIDIA PCI identity");
        if(!amd.owns(pci)) selected.insert(pci);
    }
    if(selected.empty()) {stop();return {{},started,{}};}
    if(selected.size()>64) throw std::invalid_argument("Too many NVIDIA GPU mappings");
    try {
        start();
        const auto sequence=++sequence_;
        const auto request=nlohmann::json{{"sequence",sequence},{"addresses",selected}}.dump();
        if(send(socket_,request.data(),request.size(),MSG_NOSIGNAL|MSG_DONTWAIT)!=static_cast<ssize_t>(request.size()))
            throw std::runtime_error("NVML helper request failed");
        std::array<char,nvmlMessageLimit> bytes{};
        while(SteadyClock::now()<started+timeout_) {
            pollfd descriptor{socket_,POLLIN,0};
            const int ready=poll(&descriptor,1,20);
            if(ready<0 && errno==EINTR) continue;
            if(ready<0) throw std::runtime_error("NVML helper poll failed");
            if(!ready) continue;
            const auto count=recv(socket_,bytes.data(),bytes.size(),MSG_DONTWAIT|MSG_TRUNC);
            if(count<0 && (errno==EAGAIN || errno==EINTR)) continue;
            if(count<=0) throw std::runtime_error("NVML helper disconnected or exited");
            if(static_cast<std::size_t>(count)>bytes.size()) throw std::runtime_error("NVML helper response exceeds size limit");
            const auto response=nlohmann::json::parse(bytes.data(),bytes.data()+count);
            if(response.size()!=3 || !response.at("sequence").is_number_unsigned() ||
               response.at("sequence").get<std::uint64_t>()!=sequence ||
               !response.at("temperatures").is_object() || !response.at("error").is_string())
                throw std::runtime_error("Malformed NVML helper response");
            GpuReadings result{{},started,response.at("error").get<std::string>()};
            if(result.error.size()>4096) throw std::runtime_error("Oversized NVML helper error");
            for(const auto& [pci,value]:response.at("temperatures").items()) {
                if(!selected.contains(pci) || !value.is_number_integer()) throw std::runtime_error("Unexpected NVML GPU reading");
                const auto temperature=value.get<std::int64_t>();
                if(temperature<0 || temperature>1250) throw std::runtime_error("Invalid NVML GPU temperature");
                result.temperatures[pci]=static_cast<std::int16_t>(temperature);
            }
            if(result.temperatures.size()!=selected.size() && result.error.empty())
                result.error="NVML helper omitted mapped GPU readings";
            return result;
        }
        throw std::runtime_error("NVML helper query timed out");
    } catch(...) {stop();throw;}
}
}
