#pragma once
#include "DxbcPixelGeometry.h"
#include "core/Cancellation.h"

namespace flora::dxbc_exec {
struct PixelRasterTarget {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    uint32_t width = 0, height = 1, layers = 1;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC samples{1, 0};
    bool depth = false;
};
struct PixelRasterState {
    PixelRasterTarget target;
    D3D11_RASTERIZER_DESC2 rasterizer{};
    std::vector<D3D11_VIEWPORT> viewports;
    std::vector<D3D11_RECT> scissors;
    uint32_t sampleMask = UINT32_MAX;
};
// Read-only snapshot of native rasterizer/target state. An explicit color slot
// must be bound; otherwise the first color view, depth view, or no target is used.
// depthOnly observes the DSV even when color views are bound.
PixelRasterState pixelRasterState(ID3D11DeviceContext *, std::optional<uint32_t> colorSlot = {},
                                  bool depthOnly = false);
nlohmann::json pixelRasterIdentity(const PixelRasterState &);
struct PixelInputRecord {
    // The final vector contains layer, raster sample, selected-lane raster
    // position-z bits and front-face Boolean. Position interpolation is retained
    // from the observed input; this need not be fixed-function sample depth.
    // Version 1 reserved its last two words.
    std::array<std::array<uint32_t, 4>, 9> words;
};
struct PixelRasterInputs {
    std::vector<PixelInputRecord> records;
    std::vector<uint8_t> storage;
    nlohmann::json identity;
};
// Runs only the private bridge and observation PS on the exact adapter and
// feature level. All application bindings/resources remain untouched. Depth,
// stencil, predication, blending and original PS side effects are not executed.
// Selection is a rasterization sample, which may differ from target storage
// samples when ForcedSampleCount is nonzero. Empty/ambiguous observations remain
// explicit; this does not establish original fragment eligibility.
PixelRasterInputs capturePixelRasterInputs(ID3D11Device *reference, bool warp, const PixelRasterState &,
                                           const PixelGeometryBridge &, const PixelInputShader &,
                                           uint64_t maxBytes = 256ull * 1024 * 1024,
                                           CancelCheck cancelled = {}, uint32_t timeoutMs = 5000);
} // namespace flora::dxbc_exec
