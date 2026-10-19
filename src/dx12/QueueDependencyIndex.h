#pragma once
#include "StreamDirectory.h"
#include <array>
#include <map>
#include <nlohmann/json.hpp>
#include <vector>

namespace flora::dx12 {
// Queue order and single-writer, monotonic Fence dependencies. CPU event state,
// memory hazards and command-list recording correctness are separate concerns.
class QueueDependencyIndex final {
  public:
    struct Operation {
        uint64_t record{};
        // Previous operation on this queue, then a required Fence producer.
        // Zero means no dependency; initialization records need no frame node.
        std::array<uint64_t, 2> predecessors{};
    };

  private:
    std::vector<Operation> frameOperations_;
    // Includes initialization nodes; unresolved/cyclic nodes cannot establish
    // completion of a producer on a different queue.
    std::map<uint64_t, Operation> resolvedOperations_;
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
    const std::vector<Operation> &frameOperations() const { return frameOperations_; }
    // Required CPU submission boundary for an observed completed Fence value.
    // This does not claim the GPU has actually reached that value yet.
    nlohmann::json observationDependency(uint64_t fence, uint64_t value, uint64_t record) const;
    // Strict queue/Fence dependency, including initialization. False also covers
    // missing, ambiguous or cyclic nodes; CPU record order alone is no proof.
    bool precedes(uint64_t producer, uint64_t consumer, const CancelCheck &cancelled = {}) const;
};
} // namespace flora::dx12
