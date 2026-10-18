#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <vector>
namespace flora {
struct InputMeshData {
    nlohmann::json metadata;
    std::vector<uint8_t> vertices, indices, rawVertices, rawIndices;
};
InputMeshData readDx12InputMesh(IReplayController &, const ActionDescription &, uint32_t instance, uint32_t attribute);
}
