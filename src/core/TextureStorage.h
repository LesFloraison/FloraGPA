#pragma once
#include "TextureEdits.h"
namespace flora {
struct TexturePlane {
    std::string name;
    uint32_t format{}, width{}, height{}, rowPitch{};
    uint64_t offset{}, size{};
};
std::optional<TexturePlane> texturePlane(const Resource &resource, uint32_t mip, uint32_t layer,
                                         const std::string &plane, std::optional<uint32_t> typedFormat = {});
uint32_t defaultTextureFormat(uint32_t format);
std::vector<uint8_t> textureDds(const Resource &resource, Bytes storage);
struct CapturedLuma {
    Resource resource;
    std::vector<uint8_t> bytes;
    uint64_t sourceOffset{}, sourceRowPitch{}, sourceLayerSize{};
};
CapturedLuma capturedLuma(const Resource &resource, Bytes captured, uint32_t mip, uint32_t layer,
                          uint32_t slice, const std::string &plane, std::optional<uint32_t> typedFormat = {});
} // namespace flora
