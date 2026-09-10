#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace fan {
using Json = nlohmann::json;
struct Board {
    Json config;
    std::string path, firmware;
    bool online=false;
};
class MonitorModel {
public:
    Json config, inventory{{"controllers",Json::array()},{"gpus",Json::array()},{"errors",Json::array()}};
    std::vector<Board> boards;
    bool dirty=false;
    void load(const Json& saved);
    void merge(const Json& discovered);
    Json proposed() const;
    void map(int board, int group, const Json& pci);
};
}
