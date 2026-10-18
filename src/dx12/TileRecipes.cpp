#include "TileRecipes.h"

namespace flora::dx12 {
namespace {
using format::Reader;
template<class T> std::vector<T> array(Reader &r, UINT count) {
    if (!r.flag()) return {};
    const auto entries = r.read<uint64_t>();
    if (entries != count || entries > r.remaining() / sizeof(T)) throw std::runtime_error("Invalid tile mapping array");
    std::vector<T> out; out.reserve(size_t(entries));
    for (uint64_t i = 0; i < entries; ++i) out.push_back(r.read<T>());
    return out;
}
}
TileMappingRecipe decodeTileMapping(format::Bytes bytes) {
    static_assert(sizeof(D3D12_TILED_RESOURCE_COORDINATE) == 16 && sizeof(D3D12_TILE_REGION_SIZE) == 16);
    Reader r(bytes); TileMappingRecipe out;
    out.queue = r.read<uint64_t>(); out.resource = r.read<uint64_t>(); out.regionCount = r.read<UINT>();
    out.starts = array<D3D12_TILED_RESOURCE_COORDINATE>(r, out.regionCount);
    out.sizes = array<D3D12_TILE_REGION_SIZE>(r, out.regionCount);
    out.heap = r.read<uint64_t>(); out.rangeCount = r.read<UINT>();
    out.rangeFlags = array<D3D12_TILE_RANGE_FLAGS>(r, out.rangeCount);
    out.heapOffsets = array<UINT>(r, out.rangeCount); out.tileCounts = array<UINT>(r, out.rangeCount);
    out.flags = r.read<D3D12_TILE_MAPPING_FLAGS>(); r.end(); return out;
}
TileCopyRecipe decodeTileCopy(format::Bytes bytes) {
    Reader r(bytes); TileCopyRecipe out; out.list = r.read<uint64_t>(); out.resource = r.read<uint64_t>();
    if (!r.flag()) throw std::runtime_error("Missing tile copy coordinate"); out.start = r.read<D3D12_TILED_RESOURCE_COORDINATE>();
    if (!r.flag()) throw std::runtime_error("Missing tile copy size"); out.size = r.read<D3D12_TILE_REGION_SIZE>();
    out.buffer = r.read<uint64_t>(); out.offset = r.read<uint64_t>(); out.flags = r.read<D3D12_TILE_COPY_FLAGS>(); r.end(); return out;
}
}
