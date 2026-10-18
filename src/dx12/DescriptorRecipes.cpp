#include "DescriptorRecipes.h"

namespace flora::dx12 {
namespace {
using format::Reader;
void require(bool condition, const char *reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> void optional(Reader &r, ViewDescription &out) { if (r.flag()) out = r.read<T>(); }
template<class T> std::vector<T> array(Reader &r, UINT expected, bool defaults = false) {
    if (!r.flag()) {
        require(defaults || !expected, "Missing descriptor copy ranges");
        // Absent size arrays mean one descriptor per range.
        return std::vector<T>(expected, T(1));
    }
    const auto count = r.read<uint64_t>(); require(count == expected && count <= r.remaining() / sizeof(T), "Invalid descriptor copy array");
    std::vector<T> result; result.reserve(size_t(count)); for (uint64_t i = 0; i < count; ++i) result.push_back(r.read<T>()); return result;
}
}
DescriptorHeapRecipe decodeDescriptorHeap(format::Bytes bytes) {
    Reader r(bytes); DescriptorHeapRecipe result; result.device = r.read<uint64_t>(); require(r.flag(), "Missing descriptor heap description");
    static_assert(sizeof(D3D12_DESCRIPTOR_HEAP_DESC) == 16); result.description = r.read<D3D12_DESCRIPTOR_HEAP_DESC>();
    require(r.read<GUID>() == __uuidof(ID3D12DescriptorHeap), "Unsupported descriptor heap interface");
    if (r.flag()) { require(r.read<uint64_t>() == 1, "Invalid descriptor heap output"); result.object = r.read<uint64_t>(); }
    result.result = r.read<HRESULT>(); r.end(); return result;
}
bool isDescriptorWrite(uint64_t type) {
    return type == 0x1279384597f8cULL || type == 0x127932c396f41ULL || type == 0x12793e4a19efeULL ||
        type == 0x1279306e469b5ULL || type == 0x12793378a8487ULL || type == 0x127934a282fc7ULL;
}
DescriptorWrite decodeDescriptorWrite(uint64_t type, format::Bytes bytes) {
    require(isDescriptorWrite(type), "Record is not a descriptor write"); Reader r(bytes); DescriptorWrite result; result.type = type; result.device = r.read<uint64_t>();
    if (type != 0x1279384597f8cULL && type != 0x127934a282fc7ULL) result.resource = r.read<uint64_t>();
    if (type == 0x12793e4a19efeULL) result.counter = r.read<uint64_t>();
    static_assert(sizeof(D3D12_CONSTANT_BUFFER_VIEW_DESC) == 16 && sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) == 40 && sizeof(D3D12_UNORDERED_ACCESS_VIEW_DESC) == 40);
    static_assert(sizeof(D3D12_RENDER_TARGET_VIEW_DESC) == 24 && sizeof(D3D12_DEPTH_STENCIL_VIEW_DESC) == 24 && sizeof(D3D12_SAMPLER_DESC) == 52);
    switch (type) {
    case 0x1279384597f8cULL: optional<D3D12_CONSTANT_BUFFER_VIEW_DESC>(r, result.description); break;
    case 0x127932c396f41ULL: optional<D3D12_SHADER_RESOURCE_VIEW_DESC>(r, result.description); break;
    case 0x12793e4a19efeULL: optional<D3D12_UNORDERED_ACCESS_VIEW_DESC>(r, result.description); break;
    case 0x1279306e469b5ULL: optional<D3D12_RENDER_TARGET_VIEW_DESC>(r, result.description); break;
    case 0x12793378a8487ULL: optional<D3D12_DEPTH_STENCIL_VIEW_DESC>(r, result.description); break;
    case 0x127934a282fc7ULL: optional<D3D12_SAMPLER_DESC>(r, result.description); break;
    }
    result.destination = r.read<uint64_t>(); r.end(); return result;
}
DescriptorCopy decodeDescriptorCopy(uint64_t type, format::Bytes bytes) {
    Reader r(bytes); DescriptorCopy result; result.device = r.read<uint64_t>();
    if (type == 0x12793300ba04eULL) {
        const auto count = r.read<UINT>(); result.destinations.push_back(r.read<uint64_t>()); result.sources.push_back(r.read<uint64_t>());
        result.destinationSizes.push_back(count); result.sourceSizes.push_back(count);
    } else {
        require(type == 0x1279345e6c880ULL, "Record is not a descriptor copy");
        const auto destinations = r.read<UINT>(); result.destinations = array<uint64_t>(r, destinations); result.destinationSizes = array<UINT>(r, destinations, true);
        const auto sources = r.read<UINT>(); result.sources = array<uint64_t>(r, sources); result.sourceSizes = array<UINT>(r, sources, true);
    }
    result.type = r.read<D3D12_DESCRIPTOR_HEAP_TYPE>(); require(UINT(result.type) < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES, "Invalid descriptor heap type"); r.end(); return result;
}
}
