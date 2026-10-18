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
struct CpuFenceEvent {
    uint64_t fence{}, value{}, handle{}, registration{};
};
CpuFenceEvent decodeFenceEvent(format::Bytes bytes);
struct CpuWait {
    uint64_t record{}, handle{};
    uint32_t file{};
    // Registrations recorded since the preceding wait on this handle. These
    // are observations, not a reconstruction of Win32 reset/timeout semantics.
    std::vector<CpuFenceEvent> registrations;
};
// Plan explicit submitted-queue drains at captured CPU waits. The stream does
// not carry Win32 event creation/reset/timeout state. Null-handle registrations
// are synchronous D3D12 waits and still require their exact Fence condition.
std::vector<CpuWait> planCpuWaits(const StreamDirectory &, uint64_t lastRecord,
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
