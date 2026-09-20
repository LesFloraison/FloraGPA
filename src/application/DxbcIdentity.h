#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
struct VertexIdentityShader {
    std::vector<uint8_t> bytes;
    nlohmann::json markers;
};
VertexIdentityShader instrumentVertexIdentity(Bytes original, uint32_t instances);
} // namespace flora
