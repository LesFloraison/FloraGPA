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
    // Backend-specific, checked resource lifetime and descriptor identity.
    nlohmann::json native;
};
class IReplaySession;
std::vector<DrawResourceBinding> dx12DrawResources(const IReplaySession &, Id execution,
                                                  const nlohmann::json &pipeline);
struct Dx12TextureRange {
    uint32_t mips{}, layers{}, depth{}, planes{}, samples{}, format{};
    uint32_t count() const;
    uint32_t index(uint32_t mip, uint32_t layer, uint32_t plane) const;
};
Dx12TextureRange dx12TextureRange(const nlohmann::json &descriptor);
// Validate a resource-workspace request before starting any GPU work. The worker
// subsequently checks the captured lifetime and RenderDoc's actual description.
void validateDx12ResourceRequest(const nlohmann::json &request);
std::vector<DrawResourceBinding> drawResources(const Frame &frame, Id event,
                                               const ReplayOptions &options = {});
nlohmann::json drawResourceJson(const DrawResourceBinding &binding);
nlohmann::json drawResourceInventory(const std::vector<DrawResourceBinding> &bindings, Id event);
// One prefix replay observes both boundaries inside scoped experiment edits.
// Request: {"previews": [binding keys]}, at most 16, or an empty array for inventory only.
nlohmann::json exportDrawResources(const Frame &frame, Id event, ReplayOptions options,
                                   const nlohmann::json &request, const std::filesystem::path &directory);
} // namespace flora
