#pragma once
#include "NativeResources.h"
#include <map>

namespace flora::dx12 {
class SubmissionIndex;
struct BackBufferRecipe {
    uint64_t swapchain{}, generation{}, queue{}, object{};
    // Serialized GetBuffer index: GPA restoration can rotate it relative to
    // the live application's GetCurrentBackBufferIndex observation domain.
    UINT index{};
    D3D12_RESOURCE_DESC description{};
};
// Reconstructs offscreen resources from captured swapchain descriptions. It
// never creates a window or pretends a Present call has displayed an image.
class BackBufferIndex final {
    std::map<uint64_t, BackBufferRecipe> buffers_;
  public:
    explicit BackBufferIndex(const StreamDirectory &, const CancelCheck &cancelled = {});
    const auto &buffers() const { return buffers_; }
    // Conservative default resource suggestion, not proof of displayed pixels.
    // Uses submitted transitions on the owning queue, never a live index.
    nlohmann::json suggestFrameOutput(const StreamDirectory &, const SubmissionIndex &,
                                     const CancelCheck &cancelled = {}) const;
};
}
