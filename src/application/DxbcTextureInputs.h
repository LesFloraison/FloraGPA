#pragma once
#include "DxbcExecution.h"
#include "core/TextureEdits.h"

namespace flora::dxbc_exec {
struct TextureInput {
    Resource resource; // Canonical native base descriptor with the original GPA identity.
    Buffer storage;
    float minLod = 0;
    std::vector<TextureSubresource> subresources;
};
struct TextureInputView {
    std::optional<uint64_t> resource; // Null is a verified unbound slot.
    uint32_t format = 0, dimension = 0;
    uint32_t firstMip = 0, mipCount = 1, firstLayer = 0, layerCount = 1;
    std::optional<std::array<uint32_t, 2>> depthSlices;
    std::optional<uint32_t> planeSlice;
    std::vector<uint32_t> nativeDescriptor;
};
struct TextureInputs {
    std::map<uint64_t, TextureInput> resources;
    // Native D3D11 SRV/UAV dimensions; missing resource storage stays unavailable.
    std::map<Register, TextureInputView> views;
    Value load(uint32_t stage, const Register &, uint32_t dimension, uint32_t returns, const Value &address,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask, const std::array<int32_t, 3> &offset,
               GpuOperations *, bool &assisted) const;
};
} // namespace flora::dxbc_exec
