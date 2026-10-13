#pragma once
#include "resource_layout.hpp"

namespace flora::capture {
struct MsaaSampleAccess {
    DXGI_FORMAT storage, view;
    uint32_t components, component_bytes;
    DXGI_FORMAT stencil_view=DXGI_FORMAT_UNKNOWN;
    MsaaSampleEncoding encoding=MsaaSampleEncoding::color_bits;
};
// Integer view families preserve the storage bits of normalized, signed and
// floating color formats. Depth uses the explicit access/encoding below;
// packed/BGRA color families remain unsupported. No averaged-pixel fallback.
MsaaSampleAccess msaa_integer_access(DXGI_FORMAT format);
MsaaSampleAccess msaa_sample_access(DXGI_FORMAT format, bool depth_stencil);
MsaaSamples msaa_sample_layout(const ResourceShape&, uint32_t quality,
                              size_t byte_limit=default_resource_byte_limit, bool depth_stencil=false);
struct MsaaInitializationLayout {
    MsaaSampleAccess access;
    size_t bytes=0,planes=0;
};
bool msaa_initialization_color_format(DXGI_FORMAT) noexcept;
bool msaa_initialization_depth_format(DXGI_FORMAT) noexcept;
// Pure CPU validation before records/temporary files are allocated. Currently
// Scoped RGBA8/RGBA16 color and D32S8 depth/stencil, quality zero, 2x/4x.
MsaaInitializationLayout validate_msaa_initialization(const MsaaSamples&,
    const D3D11_TEXTURE2D_DESC&,size_t byte_limit,size_t plane_limit);
}
