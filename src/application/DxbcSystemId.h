#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
struct SystemIdShader {
    std::vector<uint8_t> bytes;
    nlohmann::json metadata;
};
// Compensate a split VS submission without changing its packed input semantics.
// systemValue is D3D_NAME_VERTEX_ID (6) or D3D_NAME_INSTANCE_ID (8).
SystemIdShader offsetSystemId(Bytes original, uint32_t offset, uint32_t systemValue);
} // namespace flora
