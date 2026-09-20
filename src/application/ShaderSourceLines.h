#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>

namespace flora {
// Exact original DXBC offsets only; absent, ambiguous and invalid debug data remain unmapped.
nlohmann::json shaderSourceLines(Bytes shader, std::optional<std::string> assembly = {});
nlohmann::json shaderLinesByOffset(const nlohmann::json &report);
namespace shader_debug {
std::string sourcePathKey(const std::string &name);
nlohmann::json inlineeSources(const std::vector<std::vector<uint8_t>> &tables,
                              const std::map<uint32_t, nlohmann::json> &checksums);
} // namespace shader_debug
} // namespace flora
