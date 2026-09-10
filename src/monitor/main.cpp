#include "TerminalUi.hpp"
#include <charconv>
#include <iostream>
#include <fstream>
#include <string_view>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "fanctl " << FAN_HOST_VERSION << '\n';
        return 0;
    }
    int port = 8787;
    std::string command, file;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            std::cout << "Usage: fanctl [status|discover|config|apply FILE] [--json] [--port PORT]\n"
                "Without a command, opens the mouse-enabled controller UI (76x24 minimum).\n"
                "discover probes USB and may reset Nanos. apply saves host mappings, not fan settings.\n";
            return 0;
        }
        if ((arg == "status" || arg == "discover" || arg == "config" || arg == "apply") && command.empty()) {
            command=arg;
            if(arg=="apply") { if(i+1>=argc) return 2; file=argv[++i]; }
        }
        else if (arg == "--json") json = true;
        else if (arg == "--port" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || end != value.data() + value.size() || port < 1 || port > 65535) return 2;
        } else { std::cerr << "Unknown option; use --help\n"; return 2; }
    }
    if (json && command.empty()) { std::cerr << "--json requires a command\n"; return 2; }
    try {
        fan::ApiClient client(port);
        if (!command.empty()) {
            nlohmann::json result;
            if(command=="status") result=client.status();
            else if(command=="discover") result=client.discover();
            else if(command=="config") result=client.config();
            else { std::ifstream input(file); if(!input) throw std::runtime_error("Cannot read config file"); input>>result; result=client.save(result); }
            std::cout << (json ? result.dump() : result.dump(2)) << '\n';
            return 0;
        }
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            std::cerr << "Interactive terminal required; use fanctl status --json for scripts\n";
            return 2;
        }
        return fan::TerminalUi(client).run();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
