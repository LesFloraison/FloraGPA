#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
struct TextureInspectionOptions {
    uint32_t mip{}, layer{}, slice{};
    std::string channel = "rgba", plane = "auto";
    double low = 0, high = 1;
    std::optional<uint32_t> sample, typedFormat;
    bool preview = true;
};
struct TextureInspection {
    nlohmann::json metadata;
    std::vector<uint8_t> storage, dds, subresource, plane, captured;
    std::optional<Image> image;
};
TextureInspection inspectTexture(Replay &replay, Id resource, const TextureInspectionOptions &options = {});
void exportTextureInspection(const TextureInspection &inspection, const std::filesystem::path &directory);
} // namespace flora
