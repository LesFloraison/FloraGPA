#pragma once
#include "SubmissionIndex.h"
#include <nlohmann/json.hpp>

namespace flora::dx12 {
// Coverage in the replay backend's serialized submission order. This does not
// certify cross-queue synchronization or reconstruct unrecorded query values.
class QueryReplayIndex final {
    std::map<uint64_t, nlohmann::json> entries_;
  public:
    QueryReplayIndex(const StreamDirectory &, const SubmissionIndex &, const CancelCheck & = {});
    nlohmann::json details(uint64_t execution) const;
    nlohmann::json firstIssue(uint64_t execution, bool before) const;
    nlohmann::json information() const;
};
}
