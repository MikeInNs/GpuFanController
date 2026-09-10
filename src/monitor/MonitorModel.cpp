#include "MonitorModel.hpp"
#include <algorithm>
#include <stdexcept>
namespace fan {
void MonitorModel::load(const Json& saved) {
    config=saved; boards.clear(); dirty=false;
    for(const auto& c:saved.at("controllers")) boards.push_back({c,"","",false});
    merge(inventory);
}
void MonitorModel::merge(const Json& discovered) {
    inventory=discovered;
    for(auto& b:boards) b.online=false;
    for(const auto& c:inventory.at("controllers")) {
        auto it=std::find_if(boards.begin(),boards.end(),[&](const auto& b){
            return c["controllerId"].is_null() ? b.config["controllerId"].is_null() && b.path==c["path"].get<std::string>() : b.config["controllerId"]==c["controllerId"];
        });
        if(it==boards.end()) {
            Json cfg{{"controllerId",c["controllerId"]},{"name","Controller "+std::to_string(boards.size()+1)},
                {"groups",Json::array({{{"index",0},{"enabled",false},{"gpuPciAddress",nullptr}},
                                      {{"index",1},{"enabled",false},{"gpuPciAddress",nullptr}}})}};
            boards.push_back({cfg,c["path"],c["firmware"],true});
        } else { it->online=true; it->path=c["path"]; it->firmware=c["firmware"]; }
    }
}
Json MonitorModel::proposed() const {
    auto result=config; result["controllers"]=Json::array();
    for(const auto& b:boards) if(!b.config["controllerId"].is_null()) result["controllers"].push_back(b.config);
    return result;
}
void MonitorModel::map(int board, int group, const Json& pci) {
    auto& current=boards.at(board).config["groups"].at(group);
    if(boards.at(board).config["controllerId"].is_null()) throw std::invalid_argument("Register this Nano before mapping a GPU");
    if(!pci.is_null()) for(const auto& b:boards) for(const auto& g:b.config["groups"])
        if(&g!=&current && g["gpuPciAddress"]==pci) throw std::invalid_argument("GPU already assigned: clear its other mapping first");
    current["gpuPciAddress"]=pci; current["enabled"]=!pci.is_null(); dirty=true;
}
}
