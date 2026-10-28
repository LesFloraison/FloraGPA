#pragma once
#include "DxbcExecutionTrace.h"
#include "NativeHistory.h"
namespace flora {
struct NativePixelDrawHistoryLimits {
    uint32_t maxCandidates = 2048, timeoutMs = 30000;
    uint64_t maxBytes = 256ull * 1024 * 1024;
    // Optional private shader evaluation has separate retained/input-copy and
    // global execution limits. It does not qualify original fragments.
    bool evaluateShaderOutputs = false;
    // Join the private shader result with measured bindings and selected-sample
    // storage. Comparison with the private GPU checkpoint remains explicit.
    bool evaluateOutputMerger = false;
    uint64_t maxShaderBytes = 256ull * 1024 * 1024, maxShaderSteps = 1000000;
    // Additional complete-draw predicates associate a witnessed system ID with
    // candidate IDs. Equality alone does not qualify an original invocation.
    bool observePrimitiveIdentity = false;
    // Repeated complete draws test each distinct candidate system ID with MAX
    // color reduction. Presence does not identify a unique original fragment.
    bool observePrimitivePresence = false;
    // Counterfactual full draws disable individual output tests. Differences
    // are observations, not original-fragment failure attribution.
    bool observePrimitiveTests = false;
};
// Runs at a private before-primitive checkpoint with measured quad inputs.
// Consumers must copy mutable snapshot storage before executing a trace.
using NativePixelInvocationObserver =
    std::function<void(uint64_t primitive, const dxbc_exec::InputSnapshot &,
                       const dxbc_exec::PixelTraceOptions &, const std::vector<uint32_t> &samples)>;
// Internal selected-texel checkpoints over a full private original-PS draw.
// Records remain private primitive storage observations, not original fragments.
// The selected texture must be bound as an RTV or DSV at this event.
nlohmann::json collectNativePixelDrawHistory(Replay &, Id event, const NativeHistorySelection &,
                                             const NativePixelDrawHistoryLimits & = {},
                                             const CancelCheck & = {},
                                             const NativePixelInvocationObserver & = {});
} // namespace flora
