#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <span>
namespace flora {
// Captured UAV Clear view/rectangles; throws if the descriptor cannot be resolved.
nlohmann::json rdcUavClearRange(const SDObject &chunk, ResourceId resource,
                               std::span<const TextureDescription> textures);
nlohmann::json rdcOperationRanges(const SDObject &chunk, ResourceId resource,
                                 std::span<const TextureDescription> textures,
                                 std::span<const ResourceDescription> resources = {});
}
