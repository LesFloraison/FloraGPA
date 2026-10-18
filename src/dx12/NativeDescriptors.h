#pragma once
#include "DescriptorRecipes.h"

namespace flora::dx12 {
struct DescriptorReference { uint64_t heap{}, creation{}; UINT index{}; D3D12_DESCRIPTOR_HEAP_TYPE type{}; };
class NativeDescriptors final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
  public:
    NativeDescriptors(NativeDevice &, const StreamDirectory &, NativeResources &, const CancelCheck &cancelled = {});
    ~NativeDescriptors();
    bool apply(uint64_t record);
    ID3D12DescriptorHeap *heap(uint64_t capturedObject, uint64_t atRecord);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint64_t capturedHandle, uint64_t atRecord, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count = 1);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint64_t capturedHandle, uint64_t atRecord);
    DescriptorReference resolve(uint64_t handle, uint64_t atRecord, bool gpu) const;
    nlohmann::json information() const;
};
}
