#include "TransferCoverage.h"
#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
namespace flora {
namespace {
const SDObject &child(const SDObject &object, const char *name) {
    const SDObject *result{};
    for (size_t i = 0; i < object.NumChildren(); ++i) {
        const auto value = object.GetChild(i);
        if (value && value->name == name) {
            if (result) throw std::runtime_error(std::string("Ambiguous transfer field: ") + name);
            result = value;
        }
    }
    if (!result) throw std::runtime_error(std::string("Missing transfer field: ") + name);
    return *result;
}
uint64_t number(const SDObject &object, const char *name) {
    const auto &value = child(object, name);
    if (value.type.basetype != SDBasic::UnsignedInteger && value.type.basetype != SDBasic::Enum &&
        value.type.basetype != SDBasic::Resource)
        throw std::runtime_error(std::string("Invalid transfer integer: ") + name);
    return value.data.basic.u;
}
uint64_t coordinate(const SDObject &object, const char *name) {
    const auto &value = child(object, name);
    if (value.type.basetype != SDBasic::SignedInteger || value.data.basic.i < 0 || value.data.basic.i > INT32_MAX)
        throw std::runtime_error(std::string("Invalid transfer rectangle: ") + name);
    return uint64_t(value.data.basic.i);
}
void color2D(const TextureDescription &texture) {
    if (texture.dimension != 2 || texture.format.type != ResourceFormatType::Regular ||
        texture.format.compType == CompType::Depth)
        throw std::runtime_error("Transfer region coverage requires a regular, single-plane 2D color format");
}
std::array<uint64_t, 2> extent(const TextureDescription &texture, uint64_t subresource) {
    color2D(texture);
    if (!texture.mips || !texture.arraysize || subresource >= uint64_t(texture.mips) * texture.arraysize)
        throw std::runtime_error("Transfer subresource is outside the supported texture range");
    const auto mip = subresource % texture.mips;
    if (mip >= 32) throw std::runtime_error("Invalid transfer mip");
    return {std::max(1U, texture.width >> mip), std::max(1U, texture.height >> mip)};
}
}
nlohmann::json rdcTransferCoverage(const SDObject &chunk, const TextureDescription &texture,
                                  std::span<const TextureDescription> textures,
                                  const Subresource &sub, uint32_t x, uint32_t y) {
    using Json = nlohmann::json;
    const std::string name(chunk.name.c_str(), chunk.name.size());
    Json result{{"status", "unavailable"}, {"source", "captured_api_arguments"}, {"api", name},
                {"scope", "Destination transfer coverage; coverage does not prove a value change."}};
    try {
        const bool whole = name.ends_with("::CopyResource"), copy = name.ends_with("::CopyTextureRegion");
        const bool region = name.ends_with("::ResolveSubresourceRegion");
        if (!whole && !copy && !region && !name.ends_with("::ResolveSubresource"))
            throw std::runtime_error("Transfer coverage is not implemented for this command");
        const auto &dst = copy ? child(chunk, "dst") : chunk;
        const auto object = number(dst, copy ? "pResource" : "pDstResource");
        result["resource_id"] = std::to_string(object);
        if (object != std::bit_cast<uint64_t>(texture.resourceId)) {
            result["status"] = "outside"; result["reason"] = "Queried resource is not the transfer destination";
            return result;
        }
        if (whole) {
            result["status"] = "covered"; result["whole_resource"] = true;
            return result;
        }
        if (copy && number(dst, "Type") != 0) // D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            throw std::runtime_error("Transfer destination is not a texture subresource");
        const auto dstSub = number(dst, copy ? "SubresourceIndex" : "DstSubresource");
        const auto dstSize = extent(texture, dstSub);
        result["destination_subresource"] = dstSub;
        result["mip"] = dstSub % texture.mips; result["layer"] = dstSub / texture.mips;
        const bool subCovered = sub.mip == dstSub % texture.mips && sub.slice == dstSub / texture.mips;
        const auto &src = copy ? child(chunk, "src") : chunk;
        const auto srcObject = number(src, copy ? "pResource" : "pSrcResource");
        result["source_resource_id"] = std::to_string(srcObject);
        std::array<uint64_t, 2> srcSize{};
        if (copy && number(src, "Type") == 1) { // D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT
            const auto &footprint = child(child(src, "PlacedFootprint"), "Footprint");
            // Reject block-compressed, planar and packed-pixel layouts: their copy
            // coordinates can use different units from the destination texels.
            const auto format = number(footprint, "Format");
            if (!((format >= 1 && format <= 65) || (format >= 87 && format <= 93)))
                throw std::runtime_error("Transfer footprint format has unsupported coordinate semantics");
            srcSize = {number(footprint, "Width"), number(footprint, "Height")};
            if (number(footprint, "Depth") != 1 || !srcSize[0] || !srcSize[1])
                throw std::runtime_error("Invalid 2D transfer source footprint");
        } else {
            if (copy && number(src, "Type") != 0) throw std::runtime_error("Unknown source copy location");
            const auto source = std::find_if(textures.begin(), textures.end(), [&](const auto &t) {
                return std::bit_cast<uint64_t>(t.resourceId) == srcObject;
            });
            if (source == textures.end()) throw std::runtime_error("Transfer source texture description is unavailable");
            const auto srcSub = number(src, copy ? "SubresourceIndex" : "SrcSubresource");
            srcSize = extent(*source, srcSub); result["source_subresource"] = srcSub;
        }
        uint64_t left{}, top{}, right = srcSize[0], bottom = srcSize[1];
        if (!copy && !region && srcSize != dstSize)
            throw std::runtime_error("Full Resolve source and destination extents differ");
        if (copy || region) {
            const auto &box = child(chunk, copy ? "pSrcBox" : "pSrcRect");
            if (box.type.basetype != SDBasic::Null) {
                const auto read = copy ? number : coordinate;
                left = read(box, "left"); top = read(box, "top");
                right = read(box, "right"); bottom = read(box, "bottom");
                if (copy && (number(box, "front") != 0 || number(box, "back") != 1))
                    throw std::runtime_error("Source box does not select one 2D slice");
            }
        }
        if (right < left || bottom < top || right > srcSize[0] || bottom > srcSize[1])
            throw std::runtime_error("Transfer source rectangle exceeds the source subresource");
        const uint64_t dx = copy || region ? number(chunk, "DstX") : 0;
        const uint64_t dy = copy || region ? number(chunk, "DstY") : 0;
        if (copy && number(chunk, "DstZ") != 0) throw std::runtime_error("Nonzero destination Z for a 2D transfer");
        const auto width = right - left, height = bottom - top;
        if (dx > dstSize[0] || dy > dstSize[1] || width > dstSize[0] - dx || height > dstSize[1] - dy)
            throw std::runtime_error("Transfer destination rectangle exceeds the destination subresource");
        result["source_rectangle"] = {left, top, right, bottom};
        result["destination_rectangle"] = {dx, dy, dx + width, dy + height};
        const bool pixelCovered = x >= dx && uint64_t(x) - dx < width && y >= dy && uint64_t(y) - dy < height;
        result["subresource_covers_selection"] = subCovered; result["rectangle_covers_pixel"] = pixelCovered;
        result["status"] = subCovered && pixelCovered ? "covered" : "outside";
    } catch (const std::exception &e) { result["reason"] = e.what(); }
    return result;
}
}
