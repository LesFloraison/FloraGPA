#pragma once
#include "NativeDevice.h"
#include <d3d12.h>
#include <optional>

namespace flora::dx12 {
struct ResourceRecipe {
    enum class Kind { Committed, Placed, Reserved, Committed3 } kind = Kind::Committed;
    uint64_t device{}, object{}, heap{}, heapOffset{}, protectedSession{};
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS heapFlags{};
    D3D12_RESOURCE_DESC description{};
    D3D12_RESOURCE_DESC1 description1{};
    D3D12_RESOURCE_STATES initialState{};
    D3D12_BARRIER_LAYOUT initialLayout{};
    std::optional<D3D12_CLEAR_VALUE> clear;
    std::vector<DXGI_FORMAT> castableFormats;
    HRESULT result{};
};
struct HeapRecipe { uint64_t device{}, object{}; D3D12_HEAP_DESC description{}; HRESULT result{}; };
bool isResourceRecipe(uint64_t type);
ResourceRecipe decodeResourceRecipe(uint64_t type, format::Bytes bytes);
HeapRecipe decodeHeapRecipe(format::Bytes bytes);

// Lazily materializes captured allocations. Placed resources share the same
// native heap. Creation-record keys preserve distinct captured lifetimes.
class NativeResources final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ID3D12Resource *materialize(uint64_t object, uint64_t atRecord);
  public:
    NativeResources(NativeDevice &, const StreamDirectory &, uint64_t allocationLimit, const CancelCheck &cancelled = {});
    ~NativeResources();
    uint64_t creationRecord(uint64_t object, uint64_t atRecord) const;
    ID3D12Resource *resource(uint64_t object, uint64_t atRecord);
    // Reserve before materialization. All ordinary resource/address access is
    // rejected, including writes, barriers, aliases and descriptor bindings.
    void isolateQueryReadback(uint64_t creation);
    ID3D12Resource *queryDestination(uint64_t object, uint64_t atRecord);
    void queryWrite(uint64_t object, uint64_t atRecord, uint64_t offset, uint64_t bytes, bool available);
    ID3D12Resource *readableBuffer(uint64_t object, uint64_t atRecord, uint64_t offset, uint64_t bytes);
    ID3D12Heap *tileHeap(uint64_t object, uint64_t atRecord);
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress(uint64_t captured, uint64_t atRecord, uint64_t bytes = 1);
    ResourceRecipe recipe(uint64_t object, uint64_t atRecord) const;
    // Query placement requirements without materializing a resource or heap.
    // Sizes describe this replay device, not the original capture's adapter.
    nlohmann::json placement(uint64_t creation) const;
    void restoreMappedData(uint64_t record);
    // Caller must complete submitted GPU work before retiring a captured object.
    void releaseCompleted(uint64_t releaseRecord);
    D3D12_RESOURCE_STATES state(uint64_t object, uint64_t atRecord, UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    void transition(uint64_t object, uint64_t atRecord, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void requireState(uint64_t object, uint64_t atRecord, UINT subresource, D3D12_RESOURCE_STATES required);
    void completeSubmission(D3D12_COMMAND_LIST_TYPE queueType);
    uint64_t remainingAllocationBudget() const;
    // Retained auxiliary resources count against the same replay allocation
    // limit. The caller keeps them alive until this replay context is destroyed.
    void reserveRetainedAllocation(uint64_t bytes, uint64_t sourceRecord);
    nlohmann::json information() const;
};
}
