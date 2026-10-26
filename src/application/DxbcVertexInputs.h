#pragma once
#include "DxbcExecution.h"
#include "core/Cancellation.h"
#include <nlohmann/json.hpp>
struct ID3D11Device;

namespace flora::dxbc_exec {
struct VertexInputField {
    uint32_t reg, mask, offset;
};
struct VertexInputShader {
    std::vector<uint8_t> bytecode;
    std::vector<VertexInputField> fields;
    uint32_t stride = 0;
    std::map<Register, Value> decode(Bytes) const;
};
// Keeps the original input signature and declared components. The generated
// body only copies inputs to private unsigned SO outputs; it never runs the
// original shader's instructions, resource accesses or class instances.
VertexInputShader vertexInputShader(Bytes original, std::optional<std::array<uint32_t, 2>> systemIds = {});

struct VertexInputElement {
    std::string semantic;
    uint32_t index = 0, format = 0, slot = 0, offset = 0, classification = 0, stepRate = 0;
};
struct VertexInputBuffer {
    Bytes bytes;
    uint32_t stride = 0, offset = 0;
};
struct VertexInputDraw {
    uint32_t count = 0, instances = 1, start = 0, startInstance = 0;
    int32_t baseVertex = 0;
    // Zero means non-indexed. Index bytes include the bound IA byte offset.
    uint32_t indexFormat = 0, indexOffset = 0;
    Bytes indices;
};
struct VertexInputCapture {
    std::vector<std::map<Register, Value>> rows;
    nlohmann::json identity;
};
struct VertexInputInvocation {
    uint32_t reference = 0, instance = 0;
    std::optional<uint32_t> actualIndex;
};
// Copies supplied snapshots to a separate device on the exact reference
// adapter, preserving native IA format conversion and draw offsets. Rows are
// in instance/input-reference order with strip-cut references omitted. This
// is an input observation, not an original VS invocation/cache history.
// With a selected invocation, only one point is submitted. Instance slots are
// rebased independently and the copy shader writes the selected draw-relative
// system IDs. Supplied buffer snapshots are still copied in full.
VertexInputCapture captureVertexInputs(ID3D11Device *reference, bool warp, Bytes original,
                                       const std::vector<VertexInputElement> &,
                                       const std::map<uint32_t, VertexInputBuffer> &, const VertexInputDraw &,
                                       uint64_t maxBytes = 256ull * 1024 * 1024, CancelCheck = {},
                                       uint32_t timeoutMs = 5000,
                                       std::optional<VertexInputInvocation> selected = {});
} // namespace flora::dxbc_exec
