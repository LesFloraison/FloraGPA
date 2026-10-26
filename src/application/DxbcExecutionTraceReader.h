#pragma once
#include "DxbcExecution.h"
#include <QString>
#include <nlohmann/json.hpp>

namespace flora::dxbc_exec {
enum class TraceSpace { Buffer, Shared, Texture };
using TraceStorageKey = std::pair<TraceSpace, uint64_t>;
using TraceRegisterKey = std::pair<uint32_t, Register>;
struct TraceReadLimits {
    uint64_t maxBytes = 256 * 1024 * 1024, maxRecordBytes = 256 * 1024 * 1024;
    uint64_t maxStorageBytes = 256 * 1024 * 1024, maxSteps = 1000000;
    uint64_t maxRecords = 4000000, maxRegisters = 1000000;
};
// Owns a read lock for the journal's lifetime. The caller supplies the digest
// from the accepted job's artifact manifest and separately validates job identity.
// Positions count completed global steps; zero is the initial invocation state.
class ExecutionTraceReader {
    struct State;
    std::unique_ptr<State> state_;

  public:
    ExecutionTraceReader(const QString &path, const std::string &expectedSha256, const TraceReadLimits & = {},
                         const CancelCheck & = {});
    ~ExecutionTraceReader();
    ExecutionTraceReader(const ExecutionTraceReader &) = delete;
    ExecutionTraceReader &operator=(const ExecutionTraceReader &) = delete;
    nlohmann::json header() const;
    nlohmann::json source() const;
    nlohmann::json completion() const;
    nlohmann::json instruction(uint64_t index) const;
    nlohmann::json step(uint64_t index) const;
    // Static observation records remain in their owned format. They are inert
    // provenance, not a claim that the reader has replayed GPU operations.
    size_t observationCount() const;
    nlohmann::json observation(size_t index) const;
    uint64_t instructionCount() const;
    uint64_t size() const;
    uint64_t position() const;
    uint64_t selectedSize() const;
    // The state immediately before selected step i, or the terminal group state
    // for i == selectedSize(). Intervening work by every CS lane is included.
    uint64_t selectedBoundary(uint64_t index) const;
    // Cancellation occurs between complete steps and leaves a coherent prefix.
    void moveTo(uint64_t completedSteps, const CancelCheck & = {});
    Value registerValue(uint32_t lane, const Register &) const;
    Value constant(uint32_t slot, uint32_t element) const;
    const std::map<std::pair<uint32_t, uint32_t>, Value> &constants() const;
    const std::map<TraceRegisterKey, Value> &registers() const;
    const std::map<TraceStorageKey, Buffer> &storage() const;
    const std::map<uint64_t, Counter> &counters() const;
};
} // namespace flora::dxbc_exec
