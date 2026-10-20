#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <string_view>
namespace flora {
nlohmann::json rdcDx12ResourceReference(ResourceId id, std::string_view name);
nlohmann::json rdcResourceDescription(const ResourceDescription &resource);
nlohmann::json rdcTextureDescription(const TextureDescription &texture);
nlohmann::json rdcTextureReadbackDescription(const TextureDescription &texture, const Subresource &sub);
nlohmann::json rdcBufferDescription(const BufferDescription &buffer);
nlohmann::json rdcResourceFormat(const ResourceFormat &format);
// Preserve the original Python surrogateescape representation of the annotation
// union's single-byte character view. Other fields remain ordinary JSON.
std::string rdcInventoryText(const nlohmann::json &value);
} // namespace flora
