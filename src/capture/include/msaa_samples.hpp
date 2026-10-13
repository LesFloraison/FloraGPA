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
struct MsaaSamples {
    ObjectId resource=0;
    ResourceShape shape;
    uint32_t quality=0;
    std::vector<MsaaSamplePlane> planes;
};
struct MsaaIntegerAccess {
    DXGI_FORMAT storage, view;
    uint32_t components, component_bytes;
};
// Integer view families preserve the storage bits of normalized, signed and
// floating color formats. Depth/stencil and packed/BGRA families need a separate
// verified path; do not fall back to float conversion or averaged pixels.
MsaaIntegerAccess msaa_integer_access(DXGI_FORMAT format);
MsaaSamples msaa_sample_layout(const ResourceShape&, uint32_t quality,
                              size_t byte_limit=default_resource_byte_limit);
}
