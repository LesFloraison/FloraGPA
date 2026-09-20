#pragma once
#include "Frame.h"

namespace flora {
struct TextureSubresource {
    uint32_t mip{}, layer{}, width{}, height{}, depth{}, rowPitch{};
    uint64_t slicePitch{}, offset{}, size{};
    uint32_t index{};
};
// MSAA entries describe one sample's packed storage, not all samples concatenated.
std::vector<TextureSubresource> textureSubresources(const Resource &resource);
struct MsaaEditEncoding {
    uint32_t storage{}, access{}, display{};
    bool integerBits{};
    std::optional<std::array<uint32_t, 4>> depth;
};
MsaaEditEncoding msaaEditEncoding(const Resource &resource, std::optional<uint32_t> typedFormat = {});
struct TexturePatch {
    uint32_t mip{}, layer{};
    std::optional<uint32_t> sample, typedFormat;
    std::vector<uint8_t> bytes;
};
TextureSubresource validateTexturePatch(const Resource &resource, const TexturePatch &patch);
struct TextureOutputBinding {
    std::string role, stage;
    int slot = -1;
    Id view{};
    uint32_t format{}, dimension{}, mip{}, firstLayer{}, layerCount = 1, dsvFlags{};
    std::optional<std::array<uint32_t, 2>> depthSlices;
    bool contains(uint32_t selectedMip, uint32_t layer) const;
};
std::vector<Id> textureInputViews(const Frame &frame, const Event &event, const State &state, Id resource);
std::vector<TextureOutputBinding> textureOutputBindings(const Frame &frame, const Event &event,
                                                        const State &state, Id resource);
void validateTextureBinding(const Frame &frame, const Event &event, const State &state, Id resource,
                            const TexturePatch &patch, bool output);
} // namespace flora
