#pragma once
#include "NativeResources.h"
#include <variant>

namespace flora::dx12 {
struct DescriptorHeapRecipe { uint64_t device{}, object{}; D3D12_DESCRIPTOR_HEAP_DESC description{}; HRESULT result{}; };
using ViewDescription = std::variant<std::monostate, D3D12_CONSTANT_BUFFER_VIEW_DESC, D3D12_SHADER_RESOURCE_VIEW_DESC,
    D3D12_UNORDERED_ACCESS_VIEW_DESC, D3D12_RENDER_TARGET_VIEW_DESC, D3D12_DEPTH_STENCIL_VIEW_DESC, D3D12_SAMPLER_DESC>;
struct DescriptorWrite {
    uint64_t type{}, device{}, resource{}, counter{}, destination{};
    ViewDescription description;
};
struct DescriptorCopy {
    uint64_t device{};
    D3D12_DESCRIPTOR_HEAP_TYPE type{};
    std::vector<uint64_t> destinations, sources;
    std::vector<UINT> destinationSizes, sourceSizes;
};
DescriptorHeapRecipe decodeDescriptorHeap(format::Bytes);
bool isDescriptorWrite(uint64_t type);
DescriptorWrite decodeDescriptorWrite(uint64_t type, format::Bytes);
DescriptorCopy decodeDescriptorCopy(uint64_t type, format::Bytes);
}
