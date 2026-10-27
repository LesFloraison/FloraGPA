#pragma once
#include "NativeHistory.h"
namespace flora {
struct NativePixelDrawHistoryLimits {
    uint32_t maxCandidates = 2048, timeoutMs = 30000;
    uint64_t maxBytes = 256ull * 1024 * 1024;
};
// Internal selected-texel checkpoints over a full private original-PS draw.
// Records remain private primitive storage observations, not original fragments.
// The selected texture must be bound as an RTV or DSV at this event.
nlohmann::json collectNativePixelDrawHistory(Replay &, Id event, const NativeHistorySelection &,
                                             const NativePixelDrawHistoryLimits & = {},
                                             const CancelCheck & = {});
} // namespace flora
