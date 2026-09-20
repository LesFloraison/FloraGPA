#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
struct OutputLogShader {
    std::vector<uint8_t> bytes;
    nlohmann::json metadata;
};
// Mirrors existing output writes without adding shader inputs or outputs.
OutputLogShader instrumentOutputWrites(Bytes original, uint32_t slot, uint32_t capacity);
// Records the selected stream's Emit/Cut operations, preserving original GS execution.
OutputLogShader instrumentGeometryEmissions(Bytes original, uint32_t slot, uint32_t capacity,
                                            uint32_t stream = 0);
} // namespace flora
