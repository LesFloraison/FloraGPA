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
    uint32_t x = 0, y = 0, width = 1, height = 1;
    uint64_t maxBytes = 256ull * 1024 * 1024;
    std::vector<uint8_t> color, depth;
    // Sample-major storage; each sample contains one packed native texel.
    DXGI_SAMPLE_DESC samples{1, 0};
    uint32_t coverageMask = UINT32_MAX;
    D3D11_BLEND_DESC1 blend{};
    D3D11_DEPTH_STENCIL_DESC depthStencil{};
    std::array<float, 4> blendFactor{1, 1, 1, 1};
    uint32_t sampleMask = UINT32_MAX, stencilRef = 0, depthViewFlags = 0;
    std::optional<uint32_t> rasterDepthBits;
    uint32_t timeoutMs = 5000;
    bool frontFace = true;
    // Measure each storage sample's independent late depth, stencil and alpha
    // tests without writing targets. Does not establish original early tests.
    bool measureTests = false;
    NativePixelMergerState();
};
// Applies one private shader lane to one color/depth cell, preserving samples.
// Either target may be absent; at least one must be bound. Depth-only draws do
// not require color outputs unless alpha-to-coverage consumes target-zero alpha.
// Uses a new device on the exact reference adapter. The shader is already
// evaluated; this does not reconstruct early depth, original fragment identity,
// original raster coverage or inter-primitive side effects. Private rasterization
// uses the requested target extent and pixel location. Callers must supply the
// original location/extent to compare location-dependent alpha-to-coverage.
nlohmann::json mergeNativePixelOutput(ID3D11Device *reference, bool warp, const NativePixelMergerState &,
                                      const nlohmann::json &quadOutputs, uint32_t lane,
                                      const CancelCheck & = {});
} // namespace flora
