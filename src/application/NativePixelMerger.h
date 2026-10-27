#pragma once
#include "core/Cancellation.h"
#include <array>
#include <cstdint>
#include <d3d11_1.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace flora {
struct NativePixelMergerState {
    uint32_t colorSlot = 0, colorFormat = 0, depthFormat = 0;
    std::vector<uint8_t> color, depth;
    D3D11_BLEND_DESC1 blend{};
    D3D11_DEPTH_STENCIL_DESC depthStencil{};
    std::array<float, 4> blendFactor{1, 1, 1, 1};
    uint32_t sampleMask = UINT32_MAX, stencilRef = 0, depthViewFlags = 0;
    std::optional<uint32_t> rasterDepthBits;
    uint32_t timeoutMs = 5000;
    bool frontFace = true;
    NativePixelMergerState();
};
// Applies one private shader lane to one single-sample color/depth storage cell.
// Uses a new device on the exact reference adapter. The shader is already
// evaluated; this does not reconstruct early depth, original fragment identity,
// multisample coverage or inter-primitive side effects.
nlohmann::json mergeNativePixelOutput(ID3D11Device *reference, bool warp, const NativePixelMergerState &,
                                      const nlohmann::json &quadOutputs, uint32_t lane,
                                      const CancelCheck & = {});
} // namespace flora
