#pragma once
#include "DxbcPixelTraceInputs.h"
namespace flora {
struct NativePixelInvocationLimits {
    uint64_t maxBytes = 256ull * 1024 * 1024;
    uint32_t timeoutMs = 30000;
};
// Observes one private pre-depth quad from the supplied final geometry. The
// original shader hash, geometry bytes and adapter are retained in provenance.
// No application resources are read and original fragment eligibility is unknown.
dxbc_exec::PixelTraceOptions
captureNativePixelInvocationInputs(ID3D11Device *, bool warp, const dxbc_exec::PixelRasterState &,
                                   const PostTransformGeometry &, Bytes originalShader,
                                   uint64_t primitiveOrdinal, dxbc_exec::PixelInputRecordSelection,
                                   const NativePixelInvocationLimits & = {}, const CancelCheck & = {});
} // namespace flora
