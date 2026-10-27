#pragma once
#include "DxbcPixelInputs.h"
#include "PostTransform.h"

namespace flora::dxbc_exec {
struct PixelGeometryBridge {
    std::vector<uint8_t> vertexShader, geometryShader, vertices;
    uint32_t stride = 0, verticesPerPrimitive = 0;
    nlohmann::json identity;
};
// Selects one already expanded final primitive. The private IA layout consists
// of stride/16 UINT4 elements named FLORA_GEOMETRY with consecutive indices.
// Callers own the device, target, rasterizer and complete binding restoration.
// Primitive ordinal is a storage identity, never an inferred SV_PrimitiveID.
PixelGeometryBridge pixelGeometryBridge(const PostTransformGeometry &, const PixelInputShader &,
                                        uint64_t primitiveOrdinal,
                                        std::optional<uint32_t> uncapturedPrimitiveId = {});
// Batch already-expanded geometry without treating storage ordinals as shader
// primitive IDs. Uncaptured shader primitive IDs cannot be supplied for a range.
PixelGeometryBridge pixelGeometryRangeBridge(const PostTransformGeometry &, const PixelInputShader &,
                                             uint64_t firstPrimitive, uint64_t primitiveCount);
// Matches an unmodified PS input signature for private geometry re-execution.
// Empty shader bytes select a null PS (for example, depth-only drawing).
// The caller retains the original PS, resource bindings and output state.
PixelGeometryBridge pixelShaderGeometryRangeBridge(const PostTransformGeometry &, Bytes originalPixelShader,
                                                   uint64_t firstPrimitive, uint64_t primitiveCount,
                                                   uint64_t maxBytes = 256ull * 1024 * 1024);
} // namespace flora::dxbc_exec
