#pragma once
#include "DxbcPixelRaster.h"

namespace flora {
struct NativePixelCandidateSelection {
    uint32_t x = 0, y = 0, sample = 0, layer = 0;
    uint64_t firstPrimitive = 0, primitiveCount = 0;
    uint32_t maxRecords = 4096, timeoutMs = 5000;
    uint64_t maxBytes = 256ull * 1024 * 1024;
};
struct NativePixelCandidates {
    nlohmann::json report;
    dxbc_exec::PixelRasterInputs observations;
};
// Observes captured final geometry on the exact device's adapter. Ordinals are
// geometry storage identities, not SV_PrimitiveID or final output modifications.
// Original PS, depth/stencil, predicate and resource side effects are not run.
NativePixelCandidates captureNativePixelCandidates(ID3D11Device *, bool warp,
                                                   const dxbc_exec::PixelRasterState &,
                                                   const PostTransformGeometry &,
                                                   const NativePixelCandidateSelection &,
                                                   const CancelCheck & = {});
} // namespace flora
