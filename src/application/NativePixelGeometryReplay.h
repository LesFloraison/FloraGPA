#pragma once
#include "DxbcPixelCompletion.h"
#include "PostTransform.h"
namespace flora {
enum class NativePixelTestOverride {
    Preserve,
    DisableDepth,
    DisableStencil,
    DisableDepthStencil,
    DisableAlphaToCoverage,
    AllSamples
};
const char *nativePixelTestOverrideName(NativePixelTestOverride);
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
struct NativePixelOrdinalSelection {
    uint32_t target = 0, ordinal = 0;
    bool markReturns = false;
};
// Replaces depth/stencil with a private, initially zero stencil selector sized
// to one bound 2D RTV. References 0..254 cannot alias saturated value 255;
// larger values fail explicitly. Original PS work, coverage, predication and UAV slots
// remain. markReturns additionally applies the existing RGB completion marker.
// This is a counterfactual sample population, not original fragment eligibility.
// The requested ordinal is not a verified identity: driver selection deviations
// remain under investigation and verified_fragment_ordinal is always unknown.
// Observed storage retains original blending unless markReturns is requested.
// Single-sample coverage exports are normalized to return-time discard with an
// all-one mask export. Reports identify this transform; original eligibility is unknown.
nlohmann::json replayNativePixelOrdinal(Replay &, Id event, const NativePixelOrdinalSelection &,
                                        const std::function<void(Replay &, bool after)> &observe = {},
                                        uint64_t maxBytes = 256ull * 1024 * 1024, uint32_t timeoutMs = 30000,
                                        const CancelCheck &cancelled = {});
// Preserves the original PS body but marks its returning RGB output on one
// existing private RTV. That RTV is cleared and its blending/writes overridden;
// alpha-to-coverage, depth/stencil, sample masks and original UAV slots remain.
// Observers see private storage after diagnostic setup and after GPU completion.
nlohmann::json replayNativePixelCompletion(
    Replay &, Id event, uint32_t target, const std::function<void(Replay &, bool after)> &observe = {},
    uint64_t maxBytes = 256ull * 1024 * 1024, uint32_t timeoutMs = 30000, const CancelCheck &cancelled = {},
    std::array<uint32_t, 3> marker = {0x3f800000, 0x40000000, 0x40400000});
struct NativeDepthCompletionSelection {
    uint32_t x = 0, y = 0, layer = 0, sample = 0;
};
// Adds a private color transport to a 2D depth-only draw and reads one sample.
// Layer is relative to the bound DSV. Original depth/stencil, discard and
// coverage remain active; alpha-to-coverage and an occupied UAV slot zero are
// rejected because adding target zero would change their binding/alpha contract.
// Counts come from a separate unmodified draw on fresh private outputs. The
// diagnostic pass's own counts are retained separately; one deadline spans both.
nlohmann::json replayNativeDepthCompletion(Replay &, Id event, const NativeDepthCompletionSelection &,
                                           const std::function<void(Replay &, bool after)> &observe = {},
                                           uint64_t maxBytes = 256ull * 1024 * 1024,
                                           uint32_t timeoutMs = 30000, const CancelCheck &cancelled = {});
// Observes a predicate on the returning fragment's actual SV_PrimitiveID.
// This is a system identity, not an inferred primitive ordinal or instance ID.
nlohmann::json replayNativePixelPrimitiveMask(Replay &, Id event, uint32_t target, PixelPrimitiveMask,
                                              const std::function<void(Replay &, bool after)> &observe = {},
                                              uint64_t maxBytes = 256ull * 1024 * 1024,
                                              uint32_t timeoutMs = 30000, const CancelCheck &cancelled = {});
// Accumulates a predicate with RGB MAX on a blendable existing private RTV.
// Later nonmatching returns cannot erase an earlier matching color completion.
// Alpha is replaced as before; this does not count or order original fragments.
// Optional test overrides apply to the complete draw on fresh private outputs.
// Disabling depth/stencil also changes writes and interactions between primitives;
// differences are counterfactual observations, not original failure attribution.
nlohmann::json
replayNativePixelPrimitivePresence(Replay &, Id event, uint32_t target, PixelPrimitiveMask,
                                   const std::function<void(Replay &, bool after)> &observe = {},
                                   uint64_t maxBytes = 256ull * 1024 * 1024, uint32_t timeoutMs = 30000,
                                   const CancelCheck &cancelled = {},
                                   NativePixelTestOverride = NativePixelTestOverride::Preserve);
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
