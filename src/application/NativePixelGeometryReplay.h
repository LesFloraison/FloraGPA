#pragma once
#include "PostTransform.h"
namespace flora {
struct NativePixelGeometryRange {
    uint64_t firstPrimitive = 0, primitiveCount = 0;
    // Relative completed-primitive counts. Each boundary ends a separate Draw.
    // The final count is always included; an empty list submits one batch.
    std::vector<uint64_t> checkpoints;
    uint64_t maxBytes = 256ull * 1024 * 1024;
    uint32_t timeoutMs = 30000;
};
// Internal before-event diagnostic. It captures final geometry and runs the
// unmodified bound PS on private original outputs, preserving rasterization,
// depth/stencil, blend, sample-mask, predicate and PS resource/class bindings.
// The read-only observer sees private storage at zero and each completed count.
// Its own readback allocations and blocking calls require separate limits.
// maxBytes bounds logical geometry/bridge/output payload, not driver overhead.
// Scheduling and pre-raster side effects are not reproduced by geometry replay;
// results never establish original fragment eligibility by themselves.
nlohmann::json
replayNativePixelGeometry(Replay &, Id event, const NativePixelGeometryRange &,
                          const std::function<void(Replay &, uint64_t completedPrimitives)> &observe = {},
                          const CancelCheck &cancelled = {});
} // namespace flora
