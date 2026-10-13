#pragma once
#include "resource_layout.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace flora::capture {
struct ObjectRecord;
ResourceShape resource_shape(ID3D11Resource* native);
void remember_texture(ID3D11Resource* output, const D3D11_SUBRESOURCE_DATA* initial, const char* api) noexcept;
void track_mapping(const void* context_proxy, ID3D11Resource* resource, UINT subresource, bool mapped,
    D3D11_MAP type={},UINT flags=0,HRESULT result=S_OK,const D3D11_MAPPED_SUBRESOURCE* storage=nullptr) noexcept;
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
