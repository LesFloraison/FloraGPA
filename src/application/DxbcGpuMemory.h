#pragma once
#include "DxbcExecution.h"
#include <nlohmann/json.hpp>
struct ID3D11Device;

namespace flora::dxbc_exec {
class GpuMemory {
    struct State;
    std::unique_ptr<State> state_;
    friend class Memory;
    Value load(const Buffer &, const BufferView &, uint32_t kind, uint32_t index, uint32_t offset,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask, bool literalIndex, bool literalOffset);
    std::vector<uint8_t> store(const Buffer &, const BufferView &, uint32_t index, uint32_t offset,
                               const Value &, uint32_t mask, bool literalIndex, bool literalOffset);

  public:
    // Creates a separate device on the reference device's exact adapter. WARP
    // must be explicitly requested; no fallback or reference-context use occurs.
    GpuMemory(ID3D11Device *reference, bool warp, CancelCheck cancelled = {}, uint32_t timeoutMs = 5000);
    ~GpuMemory();
    nlohmann::json identity() const;
    uint64_t operations() const;
};
} // namespace flora::dxbc_exec
