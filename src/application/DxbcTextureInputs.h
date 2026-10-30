#pragma once
#include "DxbcExecution.h"
#include "core/TextureStorage.h"

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
struct SampleQueryInput {
    bool observed = false;
    uint32_t count = 0, resourceSamples = 1, quality = 0, forcedSamples = 0;
    bool multisampleEnabled = false;
    std::vector<std::array<uint32_t, 4>> positions;
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
    std::shared_ptr<const SampleQueryInput> queries;
    // Established from the source resource, not the diagnostic copy's descriptor.
    bool nonTiled = false;
};
// Resolve a planar SRV's actual dimensions and byte range; validate explicit plane identity.
std::optional<TexturePlane> textureInputPlane(const Resource &, const TextureInputView &, uint32_t mip,
                                              uint32_t layer);
struct SamplerInput {
    uint64_t object = 0; // Effective GPA identity before a scoped descriptor experiment.
    bool experimental = false;
    // Observed D3D11 descriptor bits. Null is a verified unbound sampler.
    std::optional<std::array<uint32_t, 13>> descriptor;
};
struct SamplingInvocation {
    Value address, ddx, ddy;
    bool active = false;
    std::optional<Value> clamp;
};
struct PixelSamplingInvocation {
    Value address, parameter;
    std::optional<Value> clamp;
    std::optional<uint32_t> literalParameter;
};
struct GatherOperation {
    uint32_t opcode = 109, component = 0;
    Value offset = Value::known({}), reference = Value::known({});
    // Destination, coordinates, programmable offset and comparison reference.
    std::array<uint32_t, 4> precision{};
    std::array<uint32_t, 3> modifiers{};
    std::array<bool, 3> literal{};
    bool refactoringAllowed = false, outputDestination = false;
};
struct TextureInputs {
    std::map<uint64_t, TextureMetadata> metadata;
    std::map<uint64_t, TextureInput> resources;
    // Native D3D11 SRV/UAV dimensions; missing resource storage stays unavailable.
    std::map<Register, TextureInputView> views;
    // Missing entries are unavailable, distinct from observed null bindings.
    std::map<Register, SamplerInput> samplers;
    // Precision codes apply to the temporary destination, coordinates and scalar parameter.
    std::array<Value, 4> samplePixelQuad(uint32_t opcode, const Register &, const Register &,
                                         uint32_t dimension, uint32_t returns,
                                         const std::array<PixelSamplingInvocation, 4> &,
                                         const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                                         const std::array<int32_t, 3> &offset, GpuOperations *,
                                         bool &assisted, const std::array<uint32_t, 3> &precision = {}) const;
    std::shared_ptr<const SampleQueryInput> rasterizerQueries;
    Value gatherTexture(uint32_t stage, const Register &, const Register &, uint32_t dimension,
                        uint32_t returns, const Value &address, const GatherOperation &,
                        const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                        const std::array<int32_t, 3> &offset, GpuOperations *, bool &assisted) const;
    Value sampleTexture(uint32_t stage, const Register &resource, const Register &sampler, uint32_t dimension,
                        uint32_t returns, const Value &address, const Value &lodOrDdx,
                        const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                        const std::array<int32_t, 3> &offset, GpuOperations *, bool &assisted,
                        const std::optional<Value> &ddy = {}, bool comparison = false,
                        const std::optional<Value> &clamp = {}, const std::array<uint32_t, 3> &precision = {},
                        bool outputDestination = false) const;
    std::optional<std::vector<Value>>
    sampleGradBatch(const Register &resource, const Register &sampler, uint32_t dimension, uint32_t returns,
                    const std::array<uint32_t, 3> &groupSize, const std::vector<SamplingInvocation> &,
                    const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                    const std::array<int32_t, 3> &offset, GpuOperations *, bool &assisted) const;
    Value sampleQuery(const Register &, uint32_t dimension, bool position, bool integer, const Value &sample,
                      bool &assisted) const;
    static uint32_t loadReturnType(uint32_t format);
    Value loadMultisample(const Register &, uint32_t dimension, uint32_t returns, const Value &address,
                          const Value &sample, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                          const std::array<int32_t, 3> &offset, bool &assisted) const;
    Value dimensions(const Register &, uint32_t dimension, const Value &mip, uint32_t mode, GpuOperations *,
                     bool &assisted) const;
    Value loadUav(const Register &, uint32_t dimension, uint32_t returns, const Value &address,
                  const std::array<uint32_t, 4> &swizzle, uint32_t mask, GpuOperations *, bool &assisted,
                  uint32_t stage = 5, const std::array<uint32_t, 2> &precision = {},
                  bool refactoringAllowed = false, bool outputDestination = false,
                  bool literalAddress = false) const;
    std::vector<MemoryChange> storeUav(const Register &, uint32_t dimension, uint32_t returns,
                                       const Value &address, const Value &, GpuOperations *, bool &assisted,
                                       uint32_t stage = 5) const;
    std::pair<Value, std::vector<MemoryChange>>
    atomicUav(const Register &, uint32_t dimension, uint32_t returns, const Value &address, uint32_t opcode,
              const Value &, const Value &comparison, GpuOperations *, bool &assisted) const;
    // Validate the complete transaction before applying a forward or reverse trace step.
    void apply(const std::vector<MemoryChange> &, bool forward = true);
    Value load(uint32_t stage, const Register &, uint32_t dimension, uint32_t returns, const Value &address,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask, const std::array<int32_t, 3> &offset,
               GpuOperations *, bool &assisted, const std::array<uint32_t, 2> &precision = {},
               bool refactoringAllowed = false, bool outputDestination = false,
               bool literalAddress = false) const;

  private:
    std::optional<Value> prepareSample(uint32_t stage, const Register &, const Register &, uint32_t dimension,
                                       uint32_t returns, const Value &address, const Value &lodOrDdx,
                                       const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                                       const std::array<int32_t, 3> &offset, const std::optional<Value> &ddy,
                                       bool comparison = false, bool gather = false,
                                       bool dimensionsOnly = false) const;
};
} // namespace flora::dxbc_exec
