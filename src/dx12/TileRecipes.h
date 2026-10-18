#pragma once
#include "NativeResources.h"

namespace flora::dx12 {
struct TileMappingRecipe {
    uint64_t queue{}, resource{}, heap{};
    UINT regionCount{}, rangeCount{};
    std::vector<D3D12_TILED_RESOURCE_COORDINATE> starts;
    std::vector<D3D12_TILE_REGION_SIZE> sizes;
    std::vector<D3D12_TILE_RANGE_FLAGS> rangeFlags;
    std::vector<UINT> heapOffsets, tileCounts;
    D3D12_TILE_MAPPING_FLAGS flags{};
};
struct TileCopyRecipe {
    uint64_t list{}, resource{}, buffer{}, offset{};
    D3D12_TILED_RESOURCE_COORDINATE start{};
    D3D12_TILE_REGION_SIZE size{};
    D3D12_TILE_COPY_FLAGS flags{};
};
TileMappingRecipe decodeTileMapping(format::Bytes);
TileCopyRecipe decodeTileCopy(format::Bytes);
}
