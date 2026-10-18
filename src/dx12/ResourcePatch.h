#pragma once
#include <cstdint>
#include <vector>

namespace flora::dx12 {
// Once before one captured ExecuteCommandLists, after preceding CPU uploads.
// All commands in that submission observe the overlay until they overwrite it.
struct ResourcePatch {
    uint64_t submission{}, creation{}, object{}, offset{};
    uint32_t subresource{};
    bool texture{};
    std::vector<uint8_t> bytes;
};
}
