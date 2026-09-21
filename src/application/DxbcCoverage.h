#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
#include <set>

namespace flora {
struct CoverageMarkerOptions {
    uint32_t slot = 0;
    bool keepAlpha = false;
    std::optional<uint32_t> arrayIndex;
    bool arrayRouted = false;
    nlohmann::json arraySource = nullptr;
};
// Preserve the original tokens and append an unused color output.
std::vector<uint8_t> addCoverageMarker(Bytes original, uint32_t slot);
// Replace a color output at main exits; only for an isolated diagnostic draw.
std::vector<uint8_t> replaceCoverageMarker(Bytes original, const CoverageMarkerOptions &options = {});
struct RelocatedShader {
    std::vector<uint8_t> bytes;
    std::set<uint32_t> declared;
};
RelocatedShader relocateShaderUavs(Bytes original, const std::map<uint32_t, uint32_t> &mapping,
                                   uint32_t slotCount = 8);
struct ReservedCoverageTarget {
    std::vector<uint8_t> bytes;
    std::map<uint32_t, uint32_t> mapping;
};
ReservedCoverageTarget reserveCoverageTarget(Bytes original, const std::set<uint32_t> &boundSlots,
                                             uint32_t slotCount = 8);
} // namespace flora
