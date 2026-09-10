#include "NvmlHelper.hpp"
#include "NvmlLibrary.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <regex>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fan {
int runNvmlHelper() {
    // Only a private socket from our parent can enter helper mode. Never open
    // HTTP, serial devices or configuration, and do not inherit daemon handlers.
    int type=0;socklen_t size=sizeof(type);
    ucred peer{};socklen_t peerSize=sizeof(peer);
    if(getsockopt(nvmlHelperFd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_SEQPACKET ||
       getsockopt(nvmlHelperFd,SOL_SOCKET,SO_PEERCRED,&peer,&peerSize) || peer.pid!=getppid()) return 2;
    const auto parent=getppid();
    if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) return 2;
    NvmlLibrary library;
    const std::regex pciPattern("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]");
    std::array<char,nvmlMessageLimit> bytes{};
    while(true) {
        const auto count=recv(nvmlHelperFd,bytes.data(),bytes.size(),MSG_TRUNC);
        if(count<0 && errno==EINTR) continue;
        if(count==0) return 0;
        if(count<0 || static_cast<std::size_t>(count)>bytes.size()) return 2;
        try {
            const auto request=nlohmann::json::parse(bytes.data(),bytes.data()+count);
            if(request.size()!=2 || !request.at("sequence").is_number_unsigned() ||
               !request.at("addresses").is_array() || request.at("addresses").size()>64) return 2;
            std::set<std::string> addresses;
            for(const auto& value:request.at("addresses")) {
                const auto pci=value.get<std::string>();
                if(!std::regex_match(pci,pciPattern) || !addresses.insert(pci).second) return 2;
            }
            auto response=library.sample(addresses);response["sequence"]=request.at("sequence");
            const auto encoded=response.dump();
            if(encoded.size()>bytes.size() || send(nvmlHelperFd,encoded.data(),encoded.size(),MSG_NOSIGNAL)!=static_cast<ssize_t>(encoded.size())) return 2;
        } catch(...) {return 2;}
    }
}
}
