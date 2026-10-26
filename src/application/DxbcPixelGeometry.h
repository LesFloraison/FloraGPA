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
} // namespace flora::dxbc_exec
