#pragma once
#include "DxbcExecution.h"
#include <nlohmann/json.hpp>
struct ID3D11Device;

namespace flora::dxbc_exec {
class GpuOperations {
    struct State;
    std::unique_ptr<State> state_;
    friend class Memory;
    friend class Lane;
    friend struct TextureInputs;
    std::optional<uint64_t> textureUavFootprint(const struct TextureInput &, const struct TextureInputView &,
                                                uint32_t dimension, uint32_t returns, const Value &address);
    std::pair<Value, std::vector<uint8_t>> textureUav(const struct TextureInput &,
                                                      const struct TextureInputView &, uint32_t dimension,
                                                      uint32_t returns, const Value &address,
                                                      const Value &value, bool store,
                                                      const std::array<uint32_t, 4> &swizzle, uint32_t mask);
    Value textureDimensions(const struct TextureMetadata &, const struct TextureInputView &, uint32_t kind,
                            uint32_t dimension, uint32_t mip, uint32_t mode);
    Value loadTexture(uint32_t stage, const struct TextureInput &, const struct TextureInputView &,
                      uint32_t dimension, uint32_t returns, const Value &address,
                      const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                      const std::array<int32_t, 3> &offset);
    Value sampleTexture(uint32_t stage, const struct TextureInput &, const struct TextureInputView &,
                        const struct SamplerInput &, uint32_t dimension, uint32_t returns,
                        const Value &address, const Value &lodOrDdx, const std::array<uint32_t, 4> &swizzle,
                        uint32_t mask, const std::array<int32_t, 3> &offset, const std::optional<Value> &ddy);
    std::vector<Value> sampleGradBatch(const struct TextureInput &, const struct TextureInputView &,
                                       const struct SamplerInput &, uint32_t dimension, uint32_t returns,
                                       const std::array<uint32_t, 3> &groupSize,
                                       const std::vector<struct SamplingInvocation> &,
                                       const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                                       const std::array<int32_t, 3> &offset);
    Value loadTyped(uint32_t stage, const Buffer &, const BufferView &, uint32_t kind, uint32_t index,
                    uint32_t returns, const std::array<uint32_t, 4> &, uint32_t mask, bool literal);
    std::vector<uint8_t> storeTyped(const Buffer &, const BufferView &, uint32_t index, uint32_t returns,
                                    const Value &, bool literal);
    std::vector<Value> instruction(uint32_t stage, const Instruction &, const std::vector<Register> &,
                                   const std::vector<uint32_t> &, const std::vector<Value> &,
                                   const std::vector<Value> &);
    Value load(const Buffer &, const BufferView &, uint32_t kind, uint32_t index, uint32_t offset,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask, bool literalIndex, bool literalOffset);
    std::vector<uint8_t> store(const Buffer &, const BufferView &, uint32_t index, uint32_t offset,
                               const Value &, uint32_t mask, bool literalIndex, bool literalOffset);

  public:
    // Creates a separate device on the reference device's exact adapter. WARP
    // must be explicitly requested; no fallback or reference-context use occurs.
    GpuOperations(ID3D11Device *reference, bool warp, CancelCheck cancelled = {}, uint32_t timeoutMs = 5000);
    ~GpuOperations();
    nlohmann::json identity() const;
    uint64_t operations() const;
};
} // namespace flora::dxbc_exec
