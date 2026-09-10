#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fan {
// These are persistence scopes, not UI pages or commands. Calibration, temporary
// modes, registration and latch acknowledgement deliberately are not draft fields.
enum class DraftScope { Host, Group1, Group2, Supply, AdapterNames };

struct DraftChange {
    std::string controllerId; // Empty only for host configuration.
    DraftScope scope;
    // Host patch is RFC 6902 for review (the host PUT uses `after`). Nano patches
    // are changed-field objects for the existing scoped API contracts.
    nlohmann::json before, after, patch;
    std::uint32_t generation;
    // Group edits also depend on the measured calibration generation.
    nlohmann::json calibrationGeneration = nullptr;
};

struct DraftConflict {
    std::string controllerId;
    DraftScope scope;
    std::string message;
};

// UI-independent, memory-only staging. No HTTP/serial/file writes. Values may be
// incomplete while typing; range/relationship validation belongs to save planning.
// In particular, navigating away must not discard a temporarily empty input.
class ConfigurationDraft {
public:
    using Json = nlohmann::json;
    void observeHost(const Json& saved);
    void editHost(const Json& desired);
    const Json& host() const { return hostDesired_; }
    const Json& latestHost() const { return hostLatest_; }

    void observeNano(const std::string& id, const Json& snapshot);
    bool hasNano(const std::string& id) const { return nanos_.contains(id); }
    const Json& baselineNano(const std::string& id) const;
    const Json& latestNano(const std::string& id) const;
    const Json& group(const std::string& id, int index) const;
    const Json& supply(const std::string& id) const;
    const Json& adapterNames(const std::string& id) const; // Null on legacy firmware.
    void editGroup(const std::string& id, int index, const std::string& key, const Json& value);
    void editSupply(const std::string& id, const std::string& key, const Json& value);
    void editAdapterNames(const std::string& id, const Json& groups);

    bool hostDirty() const;
    bool nanoDirty(const std::string& id) const;
    bool dirty() const;
    std::size_t dirtyControllers() const;
    // Returns independent copies suitable for a review. Never mutates counters.
    std::vector<DraftChange> changes() const;
    // Refreshes preserve edits; callers must present conflicts, never silently rebase.
    std::vector<DraftConflict> conflicts() const;

    // Explicit user discard adopts the latest observation, not an EEPROM rollback.
    // A save coordinator must retain uncertain write outcomes separately.
    void rebaseNano(const std::string& id, const Json& fresh);
    void acceptNano(const std::string& id, const Json& verified, DraftScope scope);
    void acceptHost(const Json& verified);
    void rebaseHost(const Json& fresh);
    void discardHost();
    void discardNano(const std::string& id);
    void discardAll();

private:
    struct Nano {
        Json baseline, latest;
        std::array<Json,2> groups;
        Json supply, names;
    };
    Json hostBaseline_ = nullptr, hostLatest_ = nullptr, hostDesired_ = nullptr;
    std::map<std::string,Nano> nanos_;
    static Nano fromSnapshot(const Json& snapshot);
    static void checkGroup(int index);
};
}
