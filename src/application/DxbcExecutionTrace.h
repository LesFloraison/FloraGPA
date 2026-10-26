#pragma once
#include "DxbcInputSnapshot.h"
#include <QString>
#include <nlohmann/json.hpp>

namespace flora::dxbc_exec {
struct GroupTraceOptions {
    std::array<uint32_t, 3> group{}, thread{};
    uint64_t maxSteps = 1000000, maxBytes = 256 * 1024 * 1024;
};
struct VertexTraceOptions {
    VertexInputSelection selection;
    uint64_t maxSteps = 1000000, maxBytes = 256 * 1024 * 1024;
};
inline constexpr const char *vertexExecutionModel = "independent-vertex-invocation/1";
// Project-owned, lossless execution records. DXBC operand kinds and opcodes are
// explicitly ISA identities, never external debugger enum values.
nlohmann::json executionTraceStep(const GroupStep &);
// Writes a bounded JSON Lines journal atomically. The snapshot's private input
// storage is consumed by execution. This low-level operation does not establish
// that the selected invocation passed dispatch bounds or predication checks.
// Callers must establish those separately before exposing a captured debug job.
nlohmann::json exportGroupExecutionTrace(const QString &path, const InputSnapshot &,
                                         const GroupTraceOptions & = {}, const CancelCheck & = {});
nlohmann::json exportVertexExecutionTrace(const QString &path, const InputSnapshot &,
                                          const VertexTraceOptions & = {}, const CancelCheck & = {});
} // namespace flora::dxbc_exec
