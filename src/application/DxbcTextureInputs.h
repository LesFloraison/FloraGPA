#pragma once
#include "DxbcExecution.h"
#include "core/TextureEdits.h"

namespace flora::dxbc_exec {
struct TextureMetadata {
    Resource resource;
    float minLod = 0;
};
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
    struct Samples {
        Resource resource;
        uint32_t format = 0, returnType = 0, dimension = 0, firstLayer = 0, layerCount = 0;
        // Shader-observed typed values, not raw texture storage. The last value is spatial OOB.
        std::vector<std::array<uint32_t, 4>> values;
    };
    std::shared_ptr<const Samples> samples;
};
struct TextureInputs {
    std::map<uint64_t, TextureMetadata> metadata;
    std::map<uint64_t, TextureInput> resources;
    // Native D3D11 SRV/UAV dimensions; missing resource storage stays unavailable.
    std::map<Register, TextureInputView> views;
    static uint32_t loadReturnType(uint32_t format);
    Value loadMultisample(const Register &, uint32_t dimension, uint32_t returns, const Value &address,
                          const Value &sample, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                          const std::array<int32_t, 3> &offset, bool &assisted) const;
    Value dimensions(const Register &, uint32_t dimension, const Value &mip, uint32_t mode, GpuOperations *,
                     bool &assisted) const;
    Value loadUav(const Register &, uint32_t dimension, uint32_t returns, const Value &address,
                  const std::array<uint32_t, 4> &swizzle, uint32_t mask, GpuOperations *,
                  bool &assisted) const;
    std::vector<MemoryChange> storeUav(const Register &, uint32_t dimension, uint32_t returns,
                                       const Value &address, const Value &, GpuOperations *,
                                       bool &assisted) const;
    // Validate the complete transaction before applying a forward or reverse trace step.
    void apply(const std::vector<MemoryChange> &, bool forward = true);
    Value load(uint32_t stage, const Register &, uint32_t dimension, uint32_t returns, const Value &address,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask, const std::array<int32_t, 3> &offset,
               GpuOperations *, bool &assisted) const;
};
} // namespace flora::dxbc_exec
