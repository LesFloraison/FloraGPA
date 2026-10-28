#pragma once
#include "PostTransform.h"
namespace flora {
namespace dxbc_exec {
struct PixelRasterState;
}
struct NativePixelGeometryRange {
    uint64_t firstPrimitive = 0, primitiveCount = 0;
    // Relative completed-primitive counts. Each boundary ends a separate Draw.
    // The final count is always included; an empty list submits one batch.
    std::vector<uint64_t> checkpoints;
    uint64_t maxBytes = 256ull * 1024 * 1024;
    uint32_t timeoutMs = 30000;
};
// Executes the original draw once with all original shader stages and bindings
// on private writable resources. Observers run before and after GPU completion.
// This measures a new replay, not the capture's original GPU scheduling.
nlohmann::json replayNativeOriginalDraw(Replay &, Id event,
                                        const std::function<void(Replay &, bool after)> &observe = {},
                                        uint64_t maxBytes = 256ull * 1024 * 1024, uint32_t timeoutMs = 30000,
                                        const CancelCheck &cancelled = {});
// Preserves the original PS body but marks its returning RGB output on one
// existing private RTV. That RTV is cleared and its blending/writes overridden;
// alpha-to-coverage, depth/stencil, sample masks and original UAV slots remain.
// Observers see private storage after diagnostic setup and after GPU completion.
nlohmann::json replayNativePixelCompletion(Replay &, Id event, uint32_t target,
                                           const std::function<void(Replay &, bool after)> &observe = {},
                                           uint64_t maxBytes = 256ull * 1024 * 1024,
                                           uint32_t timeoutMs = 30000, const CancelCheck &cancelled = {});
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
using NativePixelCheckpointPlanner =
    std::function<std::vector<uint64_t>(const PostTransformGeometry &, const dxbc_exec::PixelRasterState &)>;
// Plans checkpoints from the exact geometry used for the subsequent full draw.
// The planner is read-only and owns limits for its temporary diagnostic work.
nlohmann::json replayNativePixelGeometryCheckpoints(Replay &, Id event, const NativePixelCheckpointPlanner &,
                                                    const std::function<void(Replay &, uint64_t)> &observe,
                                                    uint64_t maxBytes = 256ull * 1024 * 1024,
                                                    uint32_t timeoutMs = 30000,
                                                    const CancelCheck &cancelled = {});
} // namespace flora
