#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace fan {
struct ConfigConflict : std::runtime_error { using std::runtime_error::runtime_error; };
class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path path);
    static std::filesystem::path defaultPath();
    static void validate(const nlohmann::json& config);
    const nlohmann::json& get() const { return config_; }
    std::string path() const { return path_.string(); }
    nlohmann::json save(nlohmann::json config);
private:
    std::filesystem::path path_;
    nlohmann::json config_;
};
}
