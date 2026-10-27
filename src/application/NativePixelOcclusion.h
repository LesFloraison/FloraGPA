#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct NativePixelOcclusionSelection {
    Id event = 0;
    // Sample selects output storage, not a ForcedSampleCount raster position.
    uint32_t colorSlot = 0, x = 0, y = 0, sample = 0;
    uint32_t timeoutMs = 5000;
};
// Owns a separate prefix replay and executes the unmodified draw on its storage.
// Narrows scissor and sample mask while preserving their original restrictions.
// This is a local occlusion diagnostic, not recovered original fragment identity:
// restricting other pixels can change shaders that communicate through UAVs.
nlohmann::json measureNativePixelOcclusion(const Frame &, ReplayOptions,
                                           const NativePixelOcclusionSelection &, const CancelCheck & = {});
} // namespace flora
