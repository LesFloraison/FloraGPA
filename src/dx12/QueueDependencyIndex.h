#pragma once
#include "StreamDirectory.h"
#include <nlohmann/json.hpp>

namespace flora::dx12 {
// Queue order and single-writer, monotonic Fence dependencies. CPU event state,
// memory hazards and command-list recording correctness are separate concerns.
class QueueDependencyIndex final {
    nlohmann::json report_;

  public:
    explicit QueueDependencyIndex(const StreamDirectory &, uint64_t lastRecord = 0,
                                  const CancelCheck &cancelled = {});
    const nlohmann::json &information() const { return report_; }
};
} // namespace flora::dx12
