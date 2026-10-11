#pragma once
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace flora::capture {
struct ObjectRecord;
struct ResourceShape {
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width=0, height=1, depth=1, mip_levels=1, array_size=1, samples=1, misc_flags=0;
};
struct SubresourceData {
    uint32_t index=0, mip=0, array_slice=0, width=0, height=1, depth=1;
    size_t row_bytes=0, rows=1, slice_bytes=0;
    std::vector<uint8_t> bytes;
};
inline constexpr size_t default_resource_byte_limit = size_t(1) << 30;
std::vector<SubresourceData> resource_layout(const ResourceShape& shape, size_t byte_limit=default_resource_byte_limit);
void copy_pitched(SubresourceData& target, const void* source, size_t row_pitch, size_t slice_pitch);
ResourceShape resource_shape(ID3D11Resource* native);
void remember_texture(ID3D11Resource* output, const D3D11_SUBRESOURCE_DATA* initial, const char* api) noexcept;
void track_mapping(const void* context_proxy, ID3D11Resource* resource, UINT subresource, bool mapped) noexcept;
struct SnapshotOptions {
    size_t byte_limit=default_resource_byte_limit;
    uint32_t timeout_ms=5000;
    std::function<bool()> cancelled;
};
struct ResourceSnapshot {
    std::shared_ptr<ObjectRecord> record;
    ResourceShape shape;
    std::vector<SubresourceData> subresources;
};
// Called at an exclusive application-context boundary, never by the IPC thread.
// No replay helpers; no binding changes except scoped predication suppression.
// Callers must synchronize all contexts that can mutate the resource.
// A retained native resource/view is accepted only if its identity is already
// registered; readback never registers an unknown native object implicitly.
ResourceSnapshot snapshot_resource(ID3D11DeviceContext* context, ID3D11Resource* resource, const SnapshotOptions& options={});
struct UavCounterSnapshot { std::shared_ptr<ObjectRecord> view; uint32_t value=0; };
UavCounterSnapshot snapshot_uav_counter(ID3D11DeviceContext* context, ID3D11UnorderedAccessView* view, const SnapshotOptions& options={});
}
