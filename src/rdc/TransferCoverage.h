#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <span>
namespace flora {
nlohmann::json rdcTransferCoverage(const SDObject &chunk, const TextureDescription &texture,
                                  std::span<const TextureDescription> textures,
                                  const Subresource &sub, uint32_t x, uint32_t y);
}
