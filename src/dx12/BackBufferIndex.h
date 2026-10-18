#pragma once
#include "NativeResources.h"
#include <map>

namespace flora::dx12 {
struct BackBufferRecipe {
    uint64_t swapchain{}, generation{}, queue{}, object{};
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
};
}
