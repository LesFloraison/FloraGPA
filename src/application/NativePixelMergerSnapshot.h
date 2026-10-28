#pragma once
#include "NativePixelMerger.h"

namespace flora {
class Replay;
struct NativePixelMergerSelection {
    // No color slot selects depth-only output. Layer is relative to each bound
    // view, matching the rasterizer's render-target array index.
    std::optional<uint32_t> colorSlot;
    uint32_t x = 0, y = 0, layer = 0;
    uint64_t maxReadbackBytes = 256ull * 1024 * 1024;
    uint32_t timeoutMs = 5000;
};
struct NativePixelMergerSnapshot {
    NativePixelMergerState state;
    nlohmann::json identity;
};
// Observes current bindings and sample-major target storage. Each target retains
// whether the transfer preserves exact bits; converted MSAA samples are not
// promoted to exact storage. Callers must
// supply separately observed depth, face and coverage before merging a lane.
// Does not advance replay or infer original fragment eligibility.
NativePixelMergerSnapshot snapshotNativePixelMergerState(Replay &, const NativePixelMergerSelection &,
                                                         const CancelCheck & = {});
} // namespace flora
