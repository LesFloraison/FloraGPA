#pragma once
#include "StreamDirectory.h"
#include <map>
#include <nlohmann/json.hpp>
#include <vector>

namespace flora::dx12 {
// Queue order and single-writer, monotonic Fence dependencies. CPU event state,
// memory hazards and command-list recording correctness are separate concerns.
class QueueDependencyIndex final {
    nlohmann::json report_;
    struct CompletionSource {
        uint64_t value{}, record{}, queue{}, requiredRecord{};
        bool resolved{};
    };
    struct FenceSources {
        uint64_t creation{}, initial{};
        bool ambiguous{};
        std::vector<CompletionSource> producers;
    };
    std::map<uint64_t, FenceSources> fenceSources_;
    uint64_t initializationCount_{};

  public:
    explicit QueueDependencyIndex(const StreamDirectory &, uint64_t lastRecord = 0,
                                  const CancelCheck &cancelled = {});
    const nlohmann::json &information() const { return report_; }
    // Required CPU submission boundary for an observed completed Fence value.
    // This does not claim the GPU has actually reached that value yet.
    nlohmann::json observationDependency(uint64_t fence, uint64_t value, uint64_t record) const;
};
} // namespace flora::dx12
