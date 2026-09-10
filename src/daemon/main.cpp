#include "ApiServer.hpp"
#include "NvmlHelper.hpp"
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string_view>
#include <thread>

int main(int argc, char** argv) try {
    if(argc==2 && std::string_view(argv[1])=="--nvml-helper") return fan::runNvmlHelper();
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "gpu-fan-controllerd " << FAN_HOST_VERSION << '\n';
        return 0;
    }
    int port = 8787;
    bool forwarding=true;
    std::filesystem::path configPath;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: gpu-fan-controllerd [--port PORT] [--config FILE] [--no-forwarding]\nNVIDIA and experimental AMD temperature forwarding and local controller API. --no-forwarding is for offline configuration/testing.\n";
        return 0;
    }
    for(int i=1;i<argc;++i) {
        const std::string_view arg(argv[i]);
        if(arg=="--no-forwarding") {forwarding=false;continue;}
        if(arg=="--config" && i+1<argc) { configPath=argv[++i]; continue; }
        if(arg!="--port" || i+1>=argc) return 2;
        std::string_view text(argv[++i]);
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
        if (error != std::errc{} || end != text.data() + text.size() || port < 0 || port > 65535) return 2;
    }
    // Handle shutdown outside a signal handler, so HTTP workers can drain safely.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) return 1;
    fan::ApiServer server(configPath.empty() ? fan::ConfigStore::defaultPath() : configPath,forwarding);
    const int bound_port = server.bind(port);
    if (bound_port < 0) { std::cerr << "Cannot bind local API port\n"; return 1; }
    std::atomic<bool> done{false};
    std::thread shutdown([&] {
        int signal=0;
        if(sigwait(&signals,&signal)==0 && !done.load()) server.stop();
    });
    std::cout << "Listening on 127.0.0.1:" << bound_port << std::endl;
    const bool ok = server.listen();
    done = true;
    // Wake the signal waiter if listen ended without an external signal.
    pthread_kill(shutdown.native_handle(),SIGTERM);
    shutdown.join();
    return ok ? 0 : 1;
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
