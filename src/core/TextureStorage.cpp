#include "TextureStorage.h"
#include <algorithm>
namespace flora {
std::vector<TextureSubresource> textureInitialSubresources(const Resource &resource, Bytes storage) {
    if (textureInfo(resource).samples != 1)
        throw std::runtime_error("Initial storage is not a per-sample MSAA initializer");
    auto subs = textureSubresources(resource);
    const auto expected = subs.back().offset + subs.back().size;
    if (storage.size() != expected)
        throw std::runtime_error("Texture initial storage length mismatch: expected " +
                                 std::to_string(expected) + ", got " + std::to_string(storage.size()));
    for (const auto &sub : subs)
        if (sub.slicePitch > UINT32_MAX)
            throw std::runtime_error("Texture initial slice pitch exceeds the native API limit");
    return subs;
}
uint32_t defaultTextureFormat(uint32_t format) {
    static const std::map<uint32_t, uint32_t> formats{
        {1, 2},   {5, 6},   {9, 10},  {15, 16}, {19, 21}, {23, 24}, {27, 28}, {33, 34},
        {39, 41}, {44, 46}, {48, 49}, {53, 54}, {60, 61}, {70, 71}, {73, 74}, {76, 77},
        {79, 80}, {82, 83}, {90, 87}, {92, 88}, {94, 95}, {97, 98}};
    auto it = formats.find(format);
    return it == formats.end() ? format : it->second;
}
std::optional<TexturePlane> texturePlane(const Resource &resource, uint32_t mip, uint32_t layer,
                                         const std::string &plane, std::optional<uint32_t> typedFormat) {
    if (plane != "auto" && plane != "y" && plane != "uv")
        throw std::runtime_error("Invalid texture plane");
    const auto info = textureInfo(resource);
    if (info.format < 103 || info.format > 105) {
        if (plane != "auto")
            throw std::runtime_error("Y/UV selection requires NV12, P010 or P016");
        return {};
    }
    const auto subs = textureSubresources(resource);
    auto it = std::find_if(subs.begin(), subs.end(),
                           [&](const auto &s) { return s.mip == mip && s.layer == layer; });
    if (it == subs.end())
        throw std::runtime_error("Texture plane mip/layer out of bounds");
    const auto y = info.format == 103 ? std::array{61u, 62u} : std::array{56u, 57u};
    const auto uv = info.format == 103 ? std::array{49u, 50u} : std::array{35u, 36u};
    const bool chroma =
        plane == "uv" || (plane == "auto" && typedFormat && (*typedFormat == uv[0] || *typedFormat == uv[1]));
    const auto formats = chroma ? uv : y;
    const auto format = typedFormat.value_or(formats[0]);
    if (format != formats[0] && format != formats[1])
        throw std::runtime_error("Typed format is incompatible with the selected plane");
    return TexturePlane{chroma ? "uv" : "y",
                        format,
                        info.width / (chroma ? 2 : 1),
                        info.height / (chroma ? 2 : 1),
                        it->rowPitch,
                        it->offset + (chroma ? uint64_t(it->rowPitch) * info.height : 0),
                        uint64_t(it->rowPitch) * (info.height / (chroma ? 2 : 1))};
}
std::vector<uint8_t> textureDds(const Resource &resource, Bytes storage) {
    const auto info = textureInfo(resource);
    const auto subs = textureSubresources(resource);
    if (info.samples != 1 || storage.size() != subs.back().offset + subs.back().size)
        throw std::runtime_error("DDS requires complete single-sample texture storage");
    const auto [pitch, rows] = pitches(info.width, info.height, info.format);
    const bool compressed =
        (info.format >= 70 && info.format <= 84) || (info.format >= 94 && info.format <= 99);
    const bool cube = (resource.desc.back() & 4) != 0;
    if (cube && (info.dimension != 3 || info.layers % 6))
        throw std::runtime_error("Invalid cube layer count");
    const auto linear = compressed ? uint64_t(pitch) * rows : pitch;
    if (linear > UINT32_MAX)
        throw std::runtime_error("DDS pitch exceeds uint32");
    uint32_t flags = 0x1007u | (compressed ? 0x80000u : 8u), caps = 0x1000, caps2 = 0;
    if (info.mips > 1) {
        flags |= 0x20000;
        caps |= 0x400008;
    }
    if (cube) {
        caps |= 8;
        caps2 |= 0xfe00;
    }
    if (info.dimension == 4) {
        flags |= 0x800000;
        caps |= 8;
        caps2 |= 0x200000;
    }
    if (info.layers > 1)
        caps |= 8;
    std::array<uint32_t, 37> header{};
    header[0] = 0x20534444;
    header[1] = 124;
    header[2] = flags;
    header[3] = info.height;
    header[4] = info.width;
    header[5] = uint32_t(linear);
    header[6] = info.dimension == 4 ? info.depth : 0;
    header[7] = info.mips;
    header[19] = 32;
    header[20] = 4;
    header[21] = 0x30315844;
    header[27] = caps;
    header[28] = caps2;
    header[32] = info.format;
    header[33] = info.dimension;
    header[34] = cube ? 4 : 0;
    header[35] = cube ? info.layers / 6 : info.layers;
    const auto first = reinterpret_cast<const uint8_t *>(header.data());
    std::vector<uint8_t> result(first, first + sizeof(header));
    result.insert(result.end(), storage.begin(), storage.end());
    return result;
}
CapturedLuma capturedLuma(const Resource &resource, Bytes captured, uint32_t mip, uint32_t layer,
                          uint32_t slice, const std::string &plane, std::optional<uint32_t> typedFormat) {
    const auto info = textureInfo(resource);
    if (resource.type != 0x85 || (info.format != 104 && info.format != 105))
        throw std::runtime_error("Legacy luma requires a P010/P016 Texture2D");
    textureSubresources(resource);
    if (mip || slice || layer >= info.layers)
        throw std::runtime_error("Legacy luma mip/layer/slice out of bounds");
    if ((plane != "auto" && plane != "y") || (typedFormat && (*typedFormat == 35 || *typedFormat == 36)))
        throw std::runtime_error(
            "Legacy GPA P010/P016 UV was not reliably saved; only captured Y is available");
    if (typedFormat && *typedFormat != 56 && *typedFormat != 57)
        throw std::runtime_error("Captured Y requires R16_UNORM or R16_UINT");
    const auto row = uint64_t(info.width) * 3, size = row * info.height;
    if (size > UINT64_MAX / info.layers || captured.size() != size * info.layers)
        throw std::runtime_error("Legacy P010/P016 GenData length mismatch");
    CapturedLuma result{resource, {}, size * layer, row, size};
    result.resource.desc = {info.width, info.height, 1, 1, typedFormat.value_or(56), 1, 0, 0, 8, 0, 0};
    for (uint32_t y = 0; y < info.height; ++y) {
        const auto bytes = captured.subspan(size_t(result.sourceOffset + row * y), size_t(info.width) * 2);
        result.bytes.insert(result.bytes.end(), bytes.begin(), bytes.end());
    }
    return result;
}
} // namespace flora
