#include "DxbcGpuOperations.h"
#include "DxbcTextureInputs.h"
#include "DxbcTypedBuffer.h"
#include <algorithm>
#include <cmath>

namespace flora::dxbc_exec {
namespace {
struct Selection {
    TypedBufferFormat format;
    std::optional<uint64_t> offset;
};
void addressMask(const Register &binding, uint32_t dimension, const Value &address) {
    const auto mask = dimension == 2 ? 1u : dimension == 3 || dimension == 7 ? 3u : 7u;
    if (binding.kind != 30 ||
        (dimension != 2 && dimension != 3 && dimension != 5 && dimension != 7 && dimension != 8))
        throw std::runtime_error("Invalid typed texture UAV dimension or binding");
    if ((address.valid & mask) != mask)
        throw std::runtime_error("Unknown typed texture UAV address");
}
Selection select(const TextureInput &texture, const TextureInputView &view, uint32_t dimension,
                 uint32_t returns, const Value &address) {
    const auto format = typedBufferFormat(view.format);
    if (!format)
        throw std::runtime_error("Typed texture UAV format validity mapping is unavailable");
    for (unsigned c = 0; c < 4; ++c) {
        const auto type = (returns >> (c * 4)) & 15;
        if (type != format->returnType && !(format->returnType == 5 && (type == 1 || type == 2)))
            throw std::runtime_error("Typed texture UAV return class disagrees with its view");
    }
    const uint32_t native = dimension == 2   ? 2
                            : dimension == 3 ? 4
                            : dimension == 5 ? 8
                            : dimension == 7 ? 3
                                             : 5;
    const auto info = textureInfo(texture.resource);
    const auto layout = textureSubresources(texture.resource);
    const bool array = dimension == 7 || dimension == 8;
    const auto &d = view.nativeDescriptor;
    if (info.dimension != (dimension == 2 || dimension == 7 ? 2u
                           : dimension == 5                 ? 4u
                                                            : 3u) ||
        info.samples != 1 || view.dimension != native || d.size() != 5 || d[0] != view.format ||
        d[1] != native || view.firstMip >= info.mips || view.mipCount != 1 || d[2] != view.firstMip ||
        view.firstLayer >= info.layers || !view.layerCount ||
        view.layerCount > info.layers - view.firstLayer ||
        (array ? d[3] != view.firstLayer ||
                     (d[4] == UINT32_MAX ? info.layers - view.firstLayer : d[4]) != view.layerCount
               : view.firstLayer != 0 || view.layerCount != 1) ||
        texture.storage.bytes.size() != layout.back().offset + layout.back().size ||
        texture.storage.valid.size() != texture.storage.bytes.size() ||
        (view.planeSlice && *view.planeSlice) || !std::isfinite(texture.minLod) || texture.minLod < 0)
        throw std::runtime_error("Invalid native typed texture UAV metadata");
    const auto mipDepth = std::max(1u, info.depth >> view.firstMip);
    if (dimension == 5 &&
        (!view.depthSlices || (*view.depthSlices)[0] >= mipDepth || !(*view.depthSlices)[1] ||
         (*view.depthSlices)[1] > mipDepth - (*view.depthSlices)[0] || d[3] != (*view.depthSlices)[0] ||
         (d[4] == UINT32_MAX ? mipDepth - d[3] : d[4]) != (*view.depthSlices)[1]))
        throw std::runtime_error("Invalid native typed texture UAV depth range");
    Selection result{*format, {}};
    const auto layer = dimension == 7 ? address.bits[1] : dimension == 8 ? address.bits[2] : 0;
    if (layer >= view.layerCount)
        return result;
    const auto &sub = layout.at((view.firstLayer + layer) * info.mips + view.firstMip);
    if (sub.rowPitch != uint64_t(sub.width) * format->bytes ||
        sub.slicePitch != uint64_t(sub.rowPitch) * sub.height)
        throw std::runtime_error("Typed texture UAV layout disagrees with its view format");
    const auto x = address.bits[0], y = dimension == 2 || dimension == 7 ? 0 : address.bits[1];
    const auto z = dimension == 5 ? address.bits[2] : 0;
    if (x >= sub.width || y >= sub.height || (dimension == 5 && z >= (*view.depthSlices)[1]))
        return result;
    result.offset = sub.offset + uint64_t(dimension == 5 ? z + (*view.depthSlices)[0] : 0) * sub.slicePitch +
                    uint64_t(y) * sub.rowPitch + uint64_t(x) * format->bytes;
    return result;
}
} // namespace
Value TextureInputs::loadUav(const Register &binding, uint32_t dimension, uint32_t returns,
                             const Value &address, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                             GpuOperations *gpu, bool &assisted) const {
    assisted = false;
    addressMask(binding, dimension, address);
    for (auto c : swizzle)
        if (c > 3)
            throw std::runtime_error("Invalid typed texture UAV swizzle");
    const auto selected = views.find(binding);
    if (selected == views.end())
        return {};
    const auto &view = selected->second;
    if (!view.resource)
        return Value::known({});
    const auto found = resources.find(*view.resource);
    if (found == resources.end())
        return {};
    const auto &texture = found->second;
    auto selection = select(texture, view, dimension, returns, address);
    if (!gpu)
        throw std::runtime_error("Typed texture UAV access requires the selected GPU");
    if (dimension == 5 && !selection.offset)
        selection.offset = gpu->textureUavFootprint(texture, view, dimension, returns, address);
    auto result = gpu->textureUav(texture, view, dimension, returns, address, {}, false, swizzle, mask).first;
    assisted = true;
    result.valid = uint8_t(mask);
    if (!selection.offset)
        return result;
    std::array<bool, 4> valid{};
    unsigned bit = 0;
    for (unsigned c = 0; c < 4; ++c) {
        const auto begin = *selection.offset + bit / 8;
        const auto end = *selection.offset + (bit + selection.format.bits[c] + 7) / 8;
        valid[c] = !selection.format.bits[c] ||
                   std::all_of(texture.storage.valid.begin() + begin, texture.storage.valid.begin() + end,
                               [](uint8_t known) { return known != 0; });
        bit += selection.format.bits[c];
    }
    for (unsigned c = 0; c < 4; ++c)
        if (!valid[swizzle[c]])
            result.valid &= uint8_t(~(1u << c));
    return result;
}
std::vector<MemoryChange> TextureInputs::storeUav(const Register &binding, uint32_t dimension,
                                                  uint32_t returns, const Value &address, const Value &value,
                                                  GpuOperations *gpu, bool &assisted) const {
    assisted = false;
    addressMask(binding, dimension, address);
    const auto selected = views.find(binding);
    if (selected == views.end())
        throw std::runtime_error("Unavailable typed texture UAV store binding");
    const auto &view = selected->second;
    if (!view.resource)
        return {};
    const auto found = resources.find(*view.resource);
    if (found == resources.end())
        throw std::runtime_error("Unavailable typed texture UAV store storage");
    const auto &texture = found->second;
    auto selection = select(texture, view, dimension, returns, address);
    if (!gpu)
        throw std::runtime_error("Typed texture UAV access requires the selected GPU");
    if (dimension == 5 && !selection.offset)
        selection.offset = gpu->textureUavFootprint(texture, view, dimension, returns, address);
    const auto after =
        gpu->textureUav(texture, view, dimension, returns, address, value, true, {0, 1, 2, 3}, 15).second;
    assisted = true;
    if (after.size() != texture.storage.bytes.size())
        throw std::runtime_error("Typed texture UAV storage size changed");
    for (size_t i = 0; i < after.size(); ++i)
        if ((!selection.offset || i < *selection.offset || i >= *selection.offset + selection.format.bytes) &&
            after[i] != texture.storage.bytes[i])
            throw std::runtime_error("Native texture UAV store changed bytes outside its selected texel");
    if (!selection.offset)
        return {};
    const auto offset = *selection.offset, end = offset + selection.format.bytes;
    MemoryChange change{*view.resource,
                        offset,
                        {texture.storage.bytes.begin() + offset, texture.storage.bytes.begin() + end},
                        {after.begin() + offset, after.begin() + end},
                        {texture.storage.valid.begin() + offset, texture.storage.valid.begin() + end},
                        std::vector<uint8_t>(selection.format.bytes, 1)};
    unsigned bit = 0;
    for (unsigned c = 0; c < 4; ++c) {
        if (!(value.valid & (1u << c)) && selection.format.bits[c])
            std::fill(change.afterValid.begin() + bit / 8,
                      change.afterValid.begin() + (bit + selection.format.bits[c] + 7) / 8, 0);
        bit += selection.format.bits[c];
    }
    return {std::move(change)};
}
void TextureInputs::apply(const std::vector<MemoryChange> &changes, bool forward) {
    uint64_t total = 0;
    std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> ranges;
    for (const auto &change : changes) {
        const auto found = resources.find(change.resource);
        if (found == resources.end())
            throw std::runtime_error("Texture trace references unavailable storage");
        const auto &storage = found->second.storage;
        const auto size = change.before.size();
        total += size;
        if (!size || total > 16 * 1024 * 1024 || change.after.size() != size ||
            change.beforeValid.size() != size || change.afterValid.size() != size ||
            storage.valid.size() != storage.bytes.size() || change.offset > storage.bytes.size() ||
            size > storage.bytes.size() - change.offset)
            throw std::runtime_error("Invalid texture trace storage range");
        const auto &expected = forward ? change.before : change.after;
        const auto &valid = forward ? change.beforeValid : change.afterValid;
        if (!std::equal(expected.begin(), expected.end(), storage.bytes.begin() + change.offset) ||
            !std::equal(valid.begin(), valid.end(), storage.valid.begin() + change.offset))
            throw std::runtime_error("Texture trace no longer matches its storage");
        for (const auto &[begin, end] : ranges[change.resource])
            if (change.offset < end && begin < change.offset + size)
                throw std::runtime_error("Overlapping texture trace changes");
        ranges[change.resource].push_back({change.offset, change.offset + size});
    }
    for (const auto &change : changes) {
        auto &storage = resources.at(change.resource).storage;
        const auto &bytes = forward ? change.after : change.before;
        const auto &valid = forward ? change.afterValid : change.beforeValid;
        std::copy(bytes.begin(), bytes.end(), storage.bytes.begin() + change.offset);
        std::copy(valid.begin(), valid.end(), storage.valid.begin() + change.offset);
    }
}
} // namespace flora::dxbc_exec
