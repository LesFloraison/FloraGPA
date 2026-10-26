#pragma once
#include "DxbcExecution.h"

namespace flora::dxbc_exec {
struct PixelInputBinding {
    std::string semantic;
    uint32_t index, system, type, reg, mask, used, precision;
};
struct PixelInputShader {
    std::vector<uint8_t> bytecode;
    uint32_t reg = 0, mask = 0;
    bool sampleFrequency = false;
    // Compact signature consumed by a private pre-raster bridge. D3D11 links
    // hardware registers as well as semantics: do not bind this shader against
    // the unmodified original producer. Rasterizer-generated values also occur
    // here; they do not require matching producer attributes.
    std::vector<PixelInputBinding> inputSignature;
    // UINT MRTs 0..3 hold the four quad lanes. MRT 4 contains pixel/role,
    // 5 sample indices, 6 coverage and 7 raw orientation diagnostics.
    // Only mask components are observations; other components are padding.
    Value decode(const std::array<uint32_t, 4> &) const;
};
// Replaces the body with a resource-free input observation. Compacts only the
// selected register, retaining its original signature, interpolation and
// minimum precision. This does not establish original fragment eligibility,
// run original side effects, or preserve original depth/discard behavior.
// The generated SM5 helper requires a feature-level-11 device.
PixelInputShader pixelInputShader(Bytes original, uint32_t inputRegister);
} // namespace flora::dxbc_exec
