#pragma once
#include "SubmissionIndex.h"
#include "QueryResultLayout.h"
#include <nlohmann/json.hpp>
#include <set>

namespace flora::dx12 {
void validateQueryDestination(uint32_t type, uint32_t count, uint64_t offset, uint64_t width);
struct QueryResultRange {
    uint32_t start{}, count{};
    uint64_t offset{}, bytes{};
    bool available{};
};
// Exact byte spans for one execution instance, never a recording-wide mask.
std::vector<QueryResultRange> queryResultRanges(const nlohmann::json &coverage);
// An isolated query readback starts entirely unknown. Only executed, completed
// query writes make bytes readable; omitted writes invalidate old contents.
class QueryReadbackState final {
    uint64_t width_{};
    std::map<uint64_t, uint64_t> known_; // half-open byte ranges
  public:
    explicit QueryReadbackState(uint64_t width) : width_(width) {}
    void write(uint64_t offset, uint64_t bytes, bool available);
    void requireReadable(uint64_t offset, uint64_t bytes) const;
    nlohmann::json information() const;
};
// Coverage in the replay backend's serialized submission order. This does not
// certify cross-queue synchronization or reconstruct unrecorded query values.
class QueryReplayIndex final {
    std::map<uint64_t, nlohmann::json> entries_;
  public:
    QueryReplayIndex(const StreamDirectory &, const SubmissionIndex &, const CancelCheck & = {});
    nlohmann::json details(uint64_t execution) const;
    nlohmann::json firstIssue(uint64_t execution, bool before) const;
    std::set<uint64_t> isolatedReadbacks(uint64_t execution, bool before) const;
    nlohmann::json information() const;
};
}
