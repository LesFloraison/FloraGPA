#include "TextureCopies.h"
#include <algorithm>
#include <bit>
namespace flora {
namespace {
uint32_t family(uint32_t f) {
    constexpr std::array<std::array<uint32_t, 2>, 20> ranges{
        {{1, 4},   {5, 8},   {9, 14},  {15, 18}, {19, 22}, {23, 25}, {27, 32}, {33, 38}, {39, 43}, {44, 47},
         {48, 52}, {53, 59}, {60, 64}, {70, 72}, {73, 75}, {76, 78}, {79, 81}, {82, 84}, {94, 96}, {97, 99}}};
    for (const auto &range : ranges)
        if (f >= range[0] && f <= range[1])
            return range[0];
    if (f == 87 || f == 90 || f == 91)
        return 90;
    if (f == 88 || f == 92 || f == 93)
        return 92;
    return f;
}
bool typeless(uint32_t f) {
    return f == family(f) && (f == 1 || f == 5 || f == 9 || f == 15 || f == 19 || f == 23 || f == 27 ||
                              f == 33 || f == 39 || f == 44 || f == 48 || f == 53 || f == 60 ||
                              (f >= 70 && f <= 84) || f == 90 || f == 92 || f == 94 || f == 97);
}
bool bc(uint32_t f) { return (f >= 70 && f <= 84) || (f >= 94 && f <= 99); }
bool specialPair(uint32_t a, uint32_t b) {
    const auto oneWay = [](uint32_t a, uint32_t b) {
        if ((a == 42 || a == 43) && b == 67)
            return true;
        if ((a == 12 || a == 14 || a == 17 || a == 18) && (b == 71 || b == 72 || b == 80 || b == 81))
            return true;
        return (a == 3 || a == 4) && (b == 74 || b == 75 || b == 77 || b == 78 || b == 83 || b == 84);
    };
    return oneWay(a, b) || oneWay(b, a);
}
struct Shape {
    TextureInfo info;
    uint32_t usage, bind, quality;
};
Shape shape(const Resource &r) {
    auto info = textureInfo(r);
    if (!info.width || !info.height || !info.depth || !info.layers || !info.samples)
        throw std::runtime_error("Texture copy descriptor has zero dimensions, layers or samples");
    const auto limit = r.type == 0x86 ? 2048u : 16384u;
    if (info.width > limit || info.height > limit || info.depth > limit || info.layers > 2048)
        throw std::runtime_error("Texture copy dimensions exceed D3D11 resource limits");
    const auto levels = std::bit_width(std::max({info.width, info.height, info.depth}));
    if (!info.mips)
        info.mips = levels;
    if (info.mips > levels || (info.samples > 1 && info.mips != 1))
        throw std::runtime_error("Texture copy mip count is invalid");
    if (!info.format || info.format > 115)
        throw std::runtime_error("Texture copy format is outside the decoded DXGI scope");
    return {info, r.desc.at(r.desc.size() - 4), r.desc.at(r.desc.size() - 3),
            r.type == 0x85 ? r.desc.at(6) : 0};
}
std::array<uint32_t, 3> extent(const Shape &s, uint32_t sub) {
    if (sub >= uint64_t(s.info.mips) * s.info.layers)
        throw std::runtime_error("Texture copy subresource index is outside the resource");
    const auto mip = sub % s.info.mips;
    return {std::max(1u, s.info.width >> mip), std::max(1u, s.info.height >> mip),
            std::max(1u, s.info.depth >> mip)};
}
} // namespace
CopyValidation validateTextureCopy(const Resource &source, const Resource &destination,
                                   const CopyCommand &c) {
    const auto s = shape(source), d = shape(destination);
    const auto sf = s.info.format, df = d.info.format;
    const auto se = extent(s, c.sourceSubresource), de = extent(d, c.destinationSubresource);
    if (c.type == 0x42) {
        if (source.type != 0x85 || destination.type != 0x85 || s.info.samples <= 1 || d.info.samples != 1 ||
            d.quality || d.usage)
            throw std::runtime_error(
                "Resolve requires an MSAA 2D source and DEFAULT single-sample 2D destination");
        if (se != de)
            throw std::runtime_error("Resolve subresource dimensions differ");
        if (!c.format || c.format > 115 || typeless(c.format) || c.format == 20 || c.format == 40 ||
            c.format == 45 || c.format == 55 || family(c.format) != family(sf) ||
            family(c.format) != family(df) || (!typeless(sf) && sf != c.format) ||
            (!typeless(df) && df != c.format))
            throw std::runtime_error("Resolve typed format is incompatible with its resources");
        return CopyValidation::Texture;
    }
    if (s.info.samples != d.info.samples || s.quality != d.quality)
        throw std::runtime_error("Texture copy sample count or quality differs");
    // The documented BC/uncompressed pairs use different coordinate units. Do not
    // apply ordinary texel extent checks or certify those paths without evidence.
    if (specialPair(sf, df))
        return CopyValidation::TextureReferences;
    if (family(sf) != family(df))
        throw std::runtime_error("Texture copy formats are incompatible");
    const bool partial = sf == 66 || sf == 68 || sf == 69 || sf >= 100;
    if (c.type == 0x3e) {
        if (se != de || s.info.layers != d.info.layers || s.info.mips != d.info.mips)
            throw std::runtime_error("CopyResource texture dimensions, layers or mip counts differ");
        return partial ? CopyValidation::TextureReferences : CopyValidation::Texture;
    }
    if (c.type != 0x40)
        throw std::runtime_error("Unsupported texture copy operation");
    auto sourceExtent = se, destinationExtent = de;
    if (bc(sf))
        for (unsigned axis = 0; axis < 2; ++axis) {
            sourceExtent[axis] = (sourceExtent[axis] + 3) & ~3u;
            destinationExtent[axis] = (destinationExtent[axis] + 3) & ~3u;
        }
    const auto box =
        c.box.value_or(std::array<uint32_t, 6>{0, 0, 0, sourceExtent[0], sourceExtent[1], sourceExtent[2]});
    const std::array<uint32_t, 3> at{c.x, c.y, c.z};
    const bool empty = box[0] >= box[3] || box[1] >= box[4] || box[2] >= box[5];
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (box[axis] > sourceExtent[axis] || box[axis + 3] > sourceExtent[axis] ||
            at[axis] > destinationExtent[axis] ||
            (!empty && uint64_t(at[axis]) + box[axis + 3] - box[axis] > destinationExtent[axis]))
            throw std::runtime_error("Texture copy region exceeds source or destination extent");
    }
    if ((source.type == 0x84 && c.y) || (source.type != 0x86 && c.z))
        throw std::runtime_error("Texture copy uses a coordinate outside its dimension");
    if (empty)
        return partial ? CopyValidation::TextureReferences : CopyValidation::Texture;
    if (source.id == destination.id && c.sourceSubresource == c.destinationSubresource)
        throw std::runtime_error("Texture region copy requires different subresources");
    if (s.info.samples > 1 || ((s.bind | d.bind) & 64)) {
        if (c.box || c.x || c.y || c.z || se != de)
            throw std::runtime_error(
                "Depth/MSAA region copy requires whole equal subresources and a null box");
    }
    if (bc(sf))
        for (unsigned axis = 0; axis < 2; ++axis) {
            const auto end = at[axis] + box[axis + 3] - box[axis];
            if (box[axis] % 4 || at[axis] % 4 || box[axis + 3] % 4 || end % 4)
                throw std::runtime_error("BC copy region is not aligned to physical storage blocks");
        }
    return partial ? CopyValidation::TextureReferences : CopyValidation::Texture;
}
} // namespace flora
