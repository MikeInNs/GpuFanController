#include "ConfigStore.hpp"
#include <fstream>
#include <set>
#include <regex>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <limits>
namespace fan {
namespace {
void require(bool valid, const char* message) { if (!valid) throw std::invalid_argument(message); }
void fields(const nlohmann::json& j, std::initializer_list<const char*> names) {
    require(j.is_object() && j.size() == names.size(), "Unexpected configuration fields");
    for (auto name : names) require(j.contains(name), "Missing configuration field");
}
}
std::filesystem::path ConfigStore::defaultPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg && std::filesystem::path(xdg).is_absolute())
        return std::filesystem::path(xdg)/"gpu-fan-controller/config.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home)/".config/gpu-fan-controller/config.json";
    throw std::runtime_error("Set --config: no user configuration directory available");
}
ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::filesystem::absolute(std::move(path))),
    config_{{"schemaVersion",1},{"revision",0},{"controllers",nlohmann::json::array()}} {
    if (std::filesystem::exists(path_)) {
        if (std::filesystem::file_size(path_) > 65536) throw std::runtime_error("Configuration exceeds 64 KiB");
        std::ifstream input(path_);
        if (!input) throw std::runtime_error("Cannot read configuration: " + path_.string());
        input >> config_;
        validate(config_);
    }
}
void ConfigStore::validate(const nlohmann::json& config) {
    if(config.contains("notifications")) {
        fields(config,{"schemaVersion","revision","controllers","notifications"});
        fields(config["notifications"],{"criticalBeep"});
        require(config["notifications"]["criticalBeep"].is_boolean(),"criticalBeep must be boolean");
    } else fields(config,{"schemaVersion","revision","controllers"});
    require(config["schemaVersion"].is_number_integer() && config["schemaVersion"] == 1,"Unsupported schemaVersion");
    require(config["revision"].is_number_integer() && config["revision"] >= 0 &&
        config["revision"] < std::numeric_limits<int>::max(),"Invalid revision");
    require(config["controllers"].is_array() && config["controllers"].size() <= 32,"Expected at most 32 controllers");
    std::set<std::string> ids, gpus;
    for (const auto& c : config["controllers"]) {
        fields(c,{"controllerId","name","groups"});
        require(c["controllerId"].is_string(),"Controller ID must be a string");
        const auto id = c["controllerId"].get<std::string>();
        require(std::regex_match(id,std::regex("[0-9a-f]{32}")) && id != std::string(32,'0'),"Invalid controller ID; register unclaimed controllers first");
        require(ids.insert(id).second,"Duplicate controller ID");
        require(c["name"].is_string(),"Controller name must be a string");
        const auto name = c["name"].get<std::string>();
        require(!name.empty() && name.size() <= 64 && name.find_first_not_of(" ") != std::string::npos,"Name must have 1-64 characters");
        for (unsigned char ch : name) require(ch >= 32 && ch < 127,"Name must use printable ASCII");
        require(c["groups"].is_array() && c["groups"].size() == 2,"Each controller must have two groups");
        for (int i=0;i<2;++i) {
            const auto& g = c["groups"][i];
            fields(g,{"index","enabled","gpuPciAddress"});
            require(g["index"].is_number_integer() && g["index"] == i,"Groups must be ordered with indexes 0 and 1");
            require(g["enabled"].is_boolean(),"enabled must be boolean");
            if (!g["enabled"].get<bool>()) require(g["gpuPciAddress"].is_null(),"Disabled groups must have null GPU");
            else {
                require(g["gpuPciAddress"].is_string(),"Enabled groups require a GPU PCI address");
                const auto pci = g["gpuPciAddress"].get<std::string>();
                require(std::regex_match(pci,std::regex("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]")),"Invalid canonical GPU PCI address");
                require(gpus.insert(pci).second,"A GPU can only be mapped to one fan group");
            }
        }
    }
}
nlohmann::json ConfigStore::save(nlohmann::json next) {
    validate(next);
    if (next["revision"] != config_["revision"]) throw ConfigConflict("Configuration changed; reload before saving");
    next["revision"] = config_["revision"].get<int>() + 1;
    const auto data = next.dump(2) + "\n";
    require(data.size() <= 65536,"Configuration exceeds 64 KiB");
    std::filesystem::create_directories(path_.parent_path());
    auto temporary = path_.string() + ".tmp.XXXXXX";
    int fd = mkstemp(temporary.data()); // Private 0600 file, on the same filesystem as the destination.
    if (fd < 0) throw std::runtime_error("Cannot create configuration temporary file");
    try {
        std::size_t offset=0;
        while (offset<data.size()) {
            auto count=write(fd,data.data()+offset,data.size()-offset);
            if(count<0 && errno==EINTR) continue;
            if(count<=0) throw std::runtime_error("Cannot write configuration");
            offset+=static_cast<std::size_t>(count);
        }
        if(fsync(fd)) throw std::runtime_error("Cannot flush configuration");
        close(fd); fd=-1;
        std::filesystem::rename(temporary,path_);
    } catch (...) { if(fd>=0) close(fd); unlink(temporary.c_str()); throw; }
    config_=std::move(next);
    // File data is durable; also request persistence of the atomic directory entry change.
    const int directory=open(path_.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory>=0) { fsync(directory); close(directory); }
    return config_;
}
}
