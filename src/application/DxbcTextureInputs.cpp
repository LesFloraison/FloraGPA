#include "DxbcTextureInputs.h"
#include "DxbcGpuOperations.h"
#include "DxbcTypedBuffer.h"
#include <algorithm>
#include <cmath>

namespace flora::dxbc_exec {
Value TextureInputs::load(uint32_t stage, const Register &binding, uint32_t dimension, uint32_t returns,
                          const Value &address, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                          const std::array<int32_t, 3> &offset, GpuOperations *gpu, bool &assisted) const {
    const uint32_t native = dimension == 2   ? 2
                            : dimension == 3 ? 4
                            : dimension == 5 ? 8
                            : dimension == 7 ? 3
                            : dimension == 8 ? 5
                                             : 0;
    if (!native || binding.kind != 7)
        throw std::runtime_error("Unsupported native texture load dimension or binding");
    const uint32_t required = dimension == 2 ? 9 : dimension == 3 || dimension == 7 ? 11 : 15;
    if ((address.valid & required) != required)
        throw std::runtime_error("Unknown DXBC texture load address");
    const auto selected = views.find(binding);
    if (selected == views.end())
        return {};
    const auto &view = selected->second;
    if (!view.resource)
        return Value::known({});
    if (view.dimension != native)
        throw std::runtime_error("DXBC texture declaration disagrees with its native view");
    const auto found = resources.find(*view.resource);
    if (found == resources.end())
        return {};
    const auto &texture = found->second;
    const auto format = typedBufferFormat(view.format);
    if (!format)
        throw std::runtime_error("Native texture load format validity mapping is unavailable");
    for (unsigned c = 0; c < 4; ++c) {
        const auto type = (returns >> (c * 4)) & 15;
        if (type != format->returnType && !(format->returnType == 5 && (type == 1 || type == 2)))
            throw std::runtime_error("DXBC texture return class disagrees with its view");
        if (swizzle[c] > 3)
            throw std::runtime_error("Invalid native texture load swizzle");
    }
    if (!gpu)
        throw std::runtime_error("Native texture loading requires GPU assistance");
    const auto info = textureInfo(texture.resource);
    const auto layout = textureSubresources(texture.resource);
    if (info.samples != 1 || texture.storage.bytes.size() != layout.back().offset + layout.back().size ||
        texture.storage.valid.size() != texture.storage.bytes.size() || !view.mipCount || !view.layerCount ||
        view.firstMip >= info.mips || view.mipCount > info.mips - view.firstMip ||
        view.firstLayer >= info.layers || view.layerCount > info.layers - view.firstLayer ||
        !std::isfinite(texture.minLod) || texture.minLod < 0 || texture.minLod > info.mips)
        throw std::runtime_error("Invalid native texture load snapshot or view range");
    if (view.nativeDescriptor.size() != 6 || view.nativeDescriptor[0] != view.format ||
        view.nativeDescriptor[1] != native || view.nativeDescriptor[2] != view.firstMip ||
        (view.nativeDescriptor[3] == UINT32_MAX ? info.mips - view.firstMip : view.nativeDescriptor[3]) !=
            view.mipCount ||
        (dimension == 7 || dimension == 8
             ? view.nativeDescriptor[4] != view.firstLayer || view.nativeDescriptor[5] != view.layerCount
             : view.firstLayer != 0 || view.layerCount != 1) ||
        (texture.minLod != 0 && !(texture.resource.desc.back() & 0x80)))
        throw std::runtime_error("Native texture load descriptor and snapshot metadata disagree");
    auto result = gpu->loadTexture(stage, texture, view, dimension, returns, address, swizzle, mask, offset);
    assisted = true;
    const auto mip = uint64_t(view.firstMip) + address.bits[3];
    const auto layer = dimension == 7 ? address.bits[1] : dimension == 8 ? address.bits[2] : 0;
    const auto x = address.bits[0] + uint32_t(offset[0]);
    const auto y =
        dimension == 3 || dimension == 5 || dimension == 8 ? address.bits[1] + uint32_t(offset[1]) : 0;
    const auto z = dimension == 5 ? address.bits[2] + uint32_t(offset[2]) : 0;
    if (address.bits[3] >= view.mipCount || layer >= view.layerCount ||
        texture.minLod > view.firstMip + view.mipCount - 1 || mip < std::floor(texture.minLod)) {
        result.valid = uint8_t(mask);
        return result;
    }
    const auto &sub = layout.at(size_t((view.firstLayer + layer) * info.mips + mip));
    if (x >= sub.width || y >= sub.height || z >= sub.depth) {
        result.valid = uint8_t(mask);
        return result;
    }
    const auto start =
        sub.offset + uint64_t(z) * sub.slicePitch + uint64_t(y) * sub.rowPitch + uint64_t(x) * format->bytes;
    if (start > texture.storage.bytes.size() || format->bytes > texture.storage.bytes.size() - start)
        throw std::runtime_error("Native texture load format exceeds its storage layout");
    std::array<bool, 4> known{};
    uint32_t bit = 0;
    for (unsigned c = 0; c < 4; ++c) {
        known[c] = !format->bits[c] ||
                   std::all_of(texture.storage.valid.begin() + start + bit / 8,
                               texture.storage.valid.begin() + start + (bit + format->bits[c] + 7) / 8,
                               [](uint8_t value) { return value != 0; });
        bit += format->bits[c];
    }
    result.valid = 0;
    for (unsigned c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && known[swizzle[c]])
            result.valid |= uint8_t(1u << c);
    return result;
}
} // namespace flora::dxbc_exec
