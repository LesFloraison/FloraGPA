#pragma once
#include "resource_layout.hpp"

namespace flora::capture {
// Separate from ordinary GenData/subresources: one tightly packed plane for
// every (array layer, sample). No resolve and no implicit sample replication.
struct MsaaSamplePlane {
    uint32_t array_slice=0, sample=0;
    size_t row_bytes=0;
    std::vector<uint8_t> bytes;
};
enum class MsaaSampleEncoding { color_bits, depth16, depth32, depth24_stencil8, depth32_stencil8 };
struct MsaaSamples {
    ObjectId resource=0;
    ResourceShape shape;
    uint32_t quality=0;
    // D32S8 planes use eight bytes per pixel: depth bits, then stencil in the
    // low byte of a zero-filled word. The unused 24 bits are not source data.
    MsaaSampleEncoding encoding=MsaaSampleEncoding::color_bits;
    std::vector<MsaaSamplePlane> planes;
};
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
}
