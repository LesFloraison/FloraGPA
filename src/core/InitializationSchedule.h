#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace flora {
struct InitializationScheduleNode {
    uint32_t id;
    uint32_t kind; // Original cache categories: state=1, resource=2, ERG=3, data=4.
    std::set<uint32_t> dependencies;
    bool initiallyReady = false;
};
struct InitializationSchedule {
    std::vector<uint32_t> order;
    std::vector<uint32_t> previousStatuses;
    std::map<uint32_t, std::set<uint32_t>> dependents;
};
// Models the pinned original scheduler, assuming every initializer succeeds.
// Fresh dependent sets only; this is not a GPU executor or edited-version model.
// Throws on duplicate/missing IDs, invalid categories, cycles or excessive work.
InitializationSchedule modelInitializationSchedule(const std::vector<InitializationScheduleNode> &nodes,
                                                   size_t workLimit = 2000000);
} // namespace flora
