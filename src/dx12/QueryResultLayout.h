#pragma once
#include <cstdint>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
namespace flora::dx12 {
// Shared by native replay, preflight and captured operation-range inspection.
inline uint32_t queryResultStride(uint32_t type) {
    switch (type) {
    case D3D12_QUERY_TYPE_OCCLUSION: case D3D12_QUERY_TYPE_BINARY_OCCLUSION: case D3D12_QUERY_TYPE_TIMESTAMP: return 8;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0: case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2: case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
        return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    default: throw std::runtime_error("Unsupported query result layout");
    }
}
}
