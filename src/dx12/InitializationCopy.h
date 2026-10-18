#pragma once
#include "NativeDevice.h"
#include <d3d12.h>

namespace flora::dx12 {
// A deliberately bounded prerequisite check: execute one captured initialization
// copy with its original buffer/heap descriptions, then compare GPU readback.
// This is not an initialized frame and cannot be used as a frame replay result.
struct InitializationCopy {
    uint64_t record{}, sourceRecord{}, destinationRecord{}, heapRecord{}, memoryLimit{};
    uint64_t source{}, destination{}, sourceOffset{}, destinationOffset{}, count{};
    D3D12_RESOURCE_DESC sourceDesc{}, destinationDesc{};
    D3D12_HEAP_PROPERTIES sourceProperties{}, destinationProperties{};
    D3D12_HEAP_FLAGS sourceFlags{}, destinationFlags{};
    D3D12_RESOURCE_STATES sourceState{}, destinationState{};
    D3D12_HEAP_DESC heapDesc{};
    uint64_t heapOffset{};
    bool placed = false;
    ResourceDataView sourceData;
};
InitializationCopy prepareInitializationCopy(const StreamDirectory &, uint64_t record,
    uint64_t memoryLimit, const CancelCheck &cancelled = {});
nlohmann::json executeInitializationCopy(NativeDevice &, const InitializationCopy &,
    const CancelCheck &cancelled = {});
}
