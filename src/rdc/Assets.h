#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json rdcResourceDescription(const ResourceDescription &resource);
nlohmann::json rdcTextureDescription(const TextureDescription &texture);
nlohmann::json rdcBufferDescription(const BufferDescription &buffer);
nlohmann::json rdcResourceFormat(const ResourceFormat &format);
// Preserve the original Python surrogateescape representation of the annotation
// union's single-byte character view. Other fields remain ordinary JSON.
std::string rdcInventoryText(const nlohmann::json &value);
} // namespace flora
