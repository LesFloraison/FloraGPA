#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <span>
namespace flora {
nlohmann::json rdcOperationRanges(const SDObject &chunk, ResourceId resource,
                                 std::span<const TextureDescription> textures);
}
