#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>

namespace flora {
enum class ResourceRole { Input, Output };
enum class ImageBoundary { Before, After };
struct ResourceRequestContext {
    std::string capture, experiment, device;
    Id event{};
    std::string cacheKey() const;
};
struct ResourceImageSelection {
    Id event{}, view{}, resource{};
    uint32_t format{}, mip{}, layer{}, slice{};
    std::optional<uint32_t> sample;
    ImageBoundary boundary = ImageBoundary::Before;
};
struct DrawResourceBinding {
    std::string key, kind, stage, error;
    ResourceRole role = ResourceRole::Input;
    uint32_t slot{};
    ResourceImageSelection image;
    bool texture{};
    uint32_t width{}, height{}, samples{}, mipEnd{}, layerEnd{}, sliceEnd{};
};
std::vector<DrawResourceBinding> drawResources(const Frame &frame, Id event,
                                               const ReplayOptions &options = {});
nlohmann::json drawResourceJson(const DrawResourceBinding &binding);
nlohmann::json drawResourceInventory(const std::vector<DrawResourceBinding> &bindings, Id event);
// One prefix replay observes both boundaries inside scoped experiment edits.
// Request: {"previews": [binding keys]}, at most 16, or an empty array for inventory only.
nlohmann::json exportDrawResources(const Frame &frame, Id event, ReplayOptions options,
                                   const nlohmann::json &request, const std::filesystem::path &directory);
} // namespace flora
