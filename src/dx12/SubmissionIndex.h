#pragma once
#include "StreamDirectory.h"
#include <optional>

namespace flora::dx12 {
struct ListRecording {
    uint64_t list{}, generation{}, begin{}, close{};
    bool hasBeginning = false, closed = false;
    std::vector<uint64_t> records;
};
struct SubmittedList {
    uint64_t submission{}, queue{}, ordinal{}, list{}, firstExecution{}, count{};
    std::optional<size_t> recording;
};
struct QueueSynchronization {
    uint64_t record{}, queue{}, fence{}, value{};
    bool wait = false;
};
struct ExecutionInstance {
    uint64_t id{}, record{}, submission{}, queue{}, list{}, ordinal{}, generation{};
    bool recordingComplete = false;
};
struct ObservationWait {
    uint64_t record{}, handle{};
    uint32_t file{};
};
// A captured CPU wait can be irrelevant to a selected GPU prefix only when
// that thread performs no state-changing calls after it within the prefix.
// This does not reconstruct event signalling or authorize future submissions.
std::vector<ObservationWait> inspectObservationWaits(const StreamDirectory &, uint64_t lastRecord,
                                                     const CancelCheck &cancelled = {});
// Indexes recorded command-list invocations. It does not certify replayability,
// resolve object aliases, or invent a global GPU order from CPU timestamps.
class SubmissionIndex {
    std::vector<ListRecording> recordings_;
    std::vector<SubmittedList> lists_;
    std::vector<QueueSynchronization> synchronization_;
    std::vector<std::optional<size_t>> recordRecording_;
    uint64_t count_{};
  public:
    explicit SubmissionIndex(const StreamDirectory &, const CancelCheck &cancelled = {});
    const auto &recordings() const { return recordings_; }
    const auto &lists() const { return lists_; }
    const auto &synchronization() const { return synchronization_; }
    uint64_t executionCount() const { return count_; }
    std::optional<size_t> recordingForRecord(uint64_t record) const;
    ExecutionInstance execution(uint64_t id) const;
};
}
