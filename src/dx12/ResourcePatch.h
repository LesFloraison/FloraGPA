#pragma once
#include <cstdint>
#include <vector>

namespace flora::dx12 {
// Applied before one captured ExecuteCommandLists (after CPU uploads), or
// immediately after one execution instance. Later commands may overwrite it.
struct ResourcePatch {
    uint64_t submission{}, creation{}, object{}, offset{};
    uint32_t subresource{};
    bool texture{};
    std::vector<uint8_t> bytes;
    // Nonzero: GPU buffer copy immediately after this execution instance.
    // The original submission field still identifies its owning submission.
    uint64_t afterExecution{};
};
}
