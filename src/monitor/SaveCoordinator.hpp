#pragma once
#include "ConfigurationDraft.hpp"
#include "fan/ApiClient.hpp"
#include <functional>
#include <map>

namespace fan {
struct SaveIo {
    using Json=nlohmann::json;
    std::function<Json()> host;
    std::function<Json(const std::string&)> snapshot;
    std::function<Json(DraftScope,const std::string&,const Json&)> write;
    static SaveIo api(const ApiClient& client);
};
struct SavePlan {
    ConfigurationDraft draft;
    std::vector<DraftChange> operations,unconfirmed;
    std::map<std::string,nlohmann::json> snapshots;
    std::map<std::string,std::string> blocked;
    nlohmann::json hostRead;
    std::string review;
};
struct SaveResult {
    ConfigurationDraft draft;
    std::vector<DraftChange> unconfirmed;
    std::string report;
};
// UI-only orchestration. Every mutation still goes through the daemon's scoped,
// generation-checked APIs; no new serial owner or live fan-control policy.
class SaveCoordinator {
public:
    explicit SaveCoordinator(SaveIo io):io_(std::move(io)){}
    SavePlan prepare(ConfigurationDraft draft,const nlohmann::json& inventory,
                     const std::vector<DraftChange>& unconfirmed={}) const;
    SaveResult execute(const SavePlan& plan) const;
    static std::string label(const DraftChange& change);
private:
    SaveIo io_;
};
}
