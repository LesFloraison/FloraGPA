#pragma once
#include "core/Frame.h"
#include <map>
#include <nlohmann/json.hpp>
#include <set>

namespace flora {
// The pinned original ERG initializer's references, not GPU resource usage.
// Unknown families and invalid records remain explicit; this does not schedule
// initialization or resolve Command List execution identities.
struct InitializationCache {
    bool complete = true;
    std::string reason;
    std::map<uint32_t, uint32_t> categories;
    std::map<uint32_t, std::set<uint32_t>> descriptors;
    bool contains(uint32_t id) const;
    nlohmann::json report() const;
};
bool originalErgType(uint16_t type);
InitializationCache initialFileCache(const Frame &frame);
nlohmann::json inspectInitializationReferences(const nlohmann::json &command,
                                               const InitializationCache *cache = nullptr);
} // namespace flora
