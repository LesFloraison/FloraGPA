#include "DxbcTextureInputs.h"
#include "DxbcGpuOperations.h"
#include "DxbcTypedBuffer.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace flora::dxbc_exec {
namespace {
struct LoadFormat {
    uint32_t bytes, returnType, width = 1, height = 1;
    // Each bit selects a contributing storage byte. Empty masks are format defaults.
    std::array<uint16_t, 4> dependencies{};
};
std::optional<LoadFormat> loadFormat(uint32_t format) {
    if (const auto typed = typedBufferFormat(format)) {
        LoadFormat result{typed->bytes, typed->returnType};
        unsigned bit = 0;
        for (unsigned c = 0; c < 4; ++c) {
            if (typed->bits[c])
                for (unsigned byte = bit / 8; byte < (bit + typed->bits[c] + 7) / 8; ++byte)
                    result.dependencies[c] |= uint16_t(1u << byte);
            bit += typed->bits[c];
        }
        return result;
    }
    static const std::map<uint32_t, LoadFormat> formats{{21, {8, 5, 1, 1, {0x000f, 0, 0, 0}}},
                                                        {22, {8, 4, 1, 1, {0, 0x0010, 0, 0}}},
                                                        {29, {4, 5, 1, 1, {1, 2, 4, 8}}},
                                                        {46, {4, 5, 1, 1, {7, 0, 0, 0}}},
                                                        {47, {4, 4, 1, 1, {0, 8, 0, 0}}},
                                                        {65, {1, 5, 1, 1, {0, 0, 0, 1}}},
                                                        {67, {4, 5, 1, 1, {11, 14, 12, 0}}},
                                                        {71, {8, 5, 4, 4, {0xff, 0xff, 0xff, 0xff}}},
                                                        {72, {8, 5, 4, 4, {0xff, 0xff, 0xff, 0xff}}},
                                                        {74, {16, 5, 4, 4, {0xff00, 0xff00, 0xff00, 0x00ff}}},
                                                        {75, {16, 5, 4, 4, {0xff00, 0xff00, 0xff00, 0x00ff}}},
                                                        {77, {16, 5, 4, 4, {0xff00, 0xff00, 0xff00, 0x00ff}}},
                                                        {78, {16, 5, 4, 4, {0xff00, 0xff00, 0xff00, 0x00ff}}},
                                                        {80, {8, 5, 4, 4, {0xff, 0, 0, 0}}},
                                                        {81, {8, 5, 4, 4, {0xff, 0, 0, 0}}},
                                                        {83, {16, 5, 4, 4, {0x00ff, 0xff00, 0, 0}}},
                                                        {84, {16, 5, 4, 4, {0x00ff, 0xff00, 0, 0}}},
                                                        {85, {2, 5, 1, 1, {2, 3, 1, 0}}},
                                                        {86, {2, 5, 1, 1, {2, 3, 1, 2}}},
                                                        {87, {4, 5, 1, 1, {4, 2, 1, 8}}},
                                                        {88, {4, 5, 1, 1, {4, 2, 1, 0}}},
                                                        {91, {4, 5, 1, 1, {4, 2, 1, 8}}},
                                                        {93, {4, 5, 1, 1, {4, 2, 1, 0}}},
                                                        {95, {16, 5, 4, 4, {0xffff, 0xffff, 0xffff, 0}}},
                                                        {96, {16, 5, 4, 4, {0xffff, 0xffff, 0xffff, 0}}},
                                                        {98, {16, 5, 4, 4, {0xffff, 0xffff, 0xffff, 0xffff}}},
                                                        {99, {16, 5, 4, 4, {0xffff, 0xffff, 0xffff, 0xffff}}},
                                                        {115, {2, 5, 1, 1, {2, 1, 1, 2}}}};
    const auto found = formats.find(format);
    return found == formats.end() ? std::nullopt : std::optional(found->second);
}
} // namespace
std::optional<TexturePlane> textureInputPlane(const Resource &resource, const TextureInputView &view,
                                              uint32_t mip, uint32_t layer) {
    const auto info = textureInfo(resource);
    if (info.format < 103 || info.format > 105) {
        if (view.planeSlice && *view.planeSlice)
            throw std::runtime_error("Unsupported plane selection for a non-planar texture");
        return {};
    }
    if (info.dimension != 3 || info.samples != 1 || info.mips != 1 || (info.width & 1) || (info.height & 1) ||
        (view.dimension != 4 && view.dimension != 5) || view.nativeDescriptor.size() != 6 ||
        (view.planeSlice && *view.planeSlice > 1))
        throw std::runtime_error("Invalid native planar SRV metadata");
    const auto plane = view.planeSlice ? (*view.planeSlice ? "uv" : "y") : "auto";
    return texturePlane(resource, mip, layer, plane, view.format);
}
uint32_t TextureInputs::loadReturnType(uint32_t format) {
    const auto description = loadFormat(format);
    return description ? description->returnType : 0;
}
std::optional<Value>
TextureInputs::prepareSample(uint32_t stage, const Register &binding, const Register &samplerBinding,
                             uint32_t dimension, uint32_t returns, const Value &address,
                             const Value &lodOrDdx, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                             const std::array<int32_t, 3> &offset, const std::optional<Value> &ddy,
                             bool comparison, bool gather, bool dimensionsOnly) const {
    const uint32_t native = dimension == 2    ? 2
                            : dimension == 3  ? 4
                            : dimension == 5  ? 8
                            : dimension == 6  ? 9
                            : dimension == 7  ? 3
                            : dimension == 8  ? 5
                            : dimension == 10 ? 10
                                              : 0;
    if (!native || binding.kind != 7 || samplerBinding.kind != 6 || mask > 15 ||
        (stage != 0 && stage != 1 && stage != 5) || (comparison && (dimension == 5 || ddy)))
        throw std::runtime_error("Invalid native explicit sampling request");
    for (unsigned c = 0; c < 4; ++c)
        if (swizzle[c] > 3 || (comparison && !gather && swizzle[c] != 0) || ((returns >> (4 * c)) & 15) < 1 ||
            (((returns >> (4 * c)) & 15) > (gather && !comparison ? 5u : 2u) &&
             ((returns >> (4 * c)) & 15) != 5))
            throw std::runtime_error("Invalid explicit sampling return class or swizzle");
    for (unsigned c = 0; c < 3; ++c)
        if (offset[c] < -8 || offset[c] > 7 || (offset[c] && (dimension == 6 || dimension == 10)))
            throw std::runtime_error("Invalid explicit sampling offset");
    const auto selected = views.find(binding);
    const auto sampler = samplers.find(samplerBinding);
    if (selected == views.end() || sampler == samplers.end())
        return Value{};
    const auto &view = selected->second;
    if (comparison && !sampler->second.descriptor)
        throw std::runtime_error("Comparison sampling with an unbound sampler is undefined");
    if ((comparison || view.resource) && sampler->second.descriptor &&
        ((((*sampler->second.descriptor)[0] & 0x180) == 0x80) != comparison))
        throw std::runtime_error("Sampling instruction and sampler comparison mode disagree");
    if (!view.resource)
        return Value::known({});
    if (view.dimension != native)
        throw std::runtime_error("Sampling declaration disagrees with its native view");
    const uint32_t sampleRequired = dimension == 2                     ? 1
                                    : dimension == 3 || dimension == 7 ? 3
                                    : dimension == 10                  ? 15
                                                                       : 7;
    const uint32_t gradientMask = dimension == 2 || dimension == 7   ? 1
                                  : dimension == 3 || dimension == 8 ? 3
                                                                     : 7;
    // LOD depends on spatial derivatives, never the array layer coordinate.
    const auto required = dimensionsOnly ? gradientMask : sampleRequired;
    const uint32_t operandMask = ddy ? gradientMask : 1;
    if ((address.valid & required) != required || (lodOrDdx.valid & operandMask) != operandMask ||
        (ddy && (ddy->valid & gradientMask) != gradientMask))
        return Value{};
    const auto found = resources.find(*view.resource);
    if (found == resources.end())
        return Value{};
    const auto &texture = found->second;
    const auto info = textureInfo(texture.resource);
    const auto layout = textureSubresources(texture.resource);
    const bool cube = dimension == 6 || dimension == 10;
    const auto returnType = loadReturnType(view.format);
    if (texture.resource.id != *view.resource ||
        (gather && !comparison ? returnType < 3 || returnType > 5 : returnType != 5) || info.samples != 1 ||
        texture.storage.bytes.size() != layout.back().offset + layout.back().size ||
        texture.storage.valid.size() != texture.storage.bytes.size() || !view.mipCount || !view.layerCount ||
        view.firstMip >= info.mips || view.mipCount > info.mips - view.firstMip ||
        view.firstLayer >= info.layers || view.layerCount > info.layers - view.firstLayer ||
        !std::isfinite(texture.minLod) || texture.minLod < 0 || texture.minLod > info.mips ||
        (cube && (!(texture.resource.desc.back() & 4) || view.firstLayer % 6 || view.layerCount % 6)))
        throw std::runtime_error("Invalid native sampling storage or view range");
    if (view.nativeDescriptor.size() != 6 || view.nativeDescriptor[0] != view.format ||
        view.nativeDescriptor[1] != native || view.nativeDescriptor[2] != view.firstMip ||
        (view.nativeDescriptor[3] == UINT32_MAX ? info.mips - view.firstMip : view.nativeDescriptor[3]) !=
            view.mipCount ||
        (dimension == 7 || dimension == 8 || dimension == 10
             ? view.nativeDescriptor[4] != view.firstLayer ||
                   uint64_t(view.nativeDescriptor[5]) * (cube ? 6 : 1) != view.layerCount
             : view.firstLayer != 0 || view.layerCount != (cube ? 6u : 1u)) ||
        (texture.minLod != 0 && !(texture.resource.desc.back() & 0x80)))
        throw std::runtime_error("Native sampling descriptor and snapshot metadata disagree");
    const auto plane = textureInputPlane(texture.resource, view, view.firstMip, view.firstLayer);
    if (dimensionsOnly)
        return std::nullopt;
    // Filtering may combine multiple mips, faces and anisotropic taps. Until a
    // dependency footprint is observed, never mark partially unknown storage known.
    if (plane) {
        for (uint32_t layer = view.firstLayer; layer < view.firstLayer + view.layerCount; ++layer) {
            const auto selectedPlane = textureInputPlane(texture.resource, view, view.firstMip, layer);
            if (!std::all_of(texture.storage.valid.begin() + selectedPlane->offset,
                             texture.storage.valid.begin() + selectedPlane->offset + selectedPlane->size,
                             [](auto value) { return value == 1; }))
                return Value{};
        }
    } else if (!std::all_of(texture.storage.valid.begin(), texture.storage.valid.end(),
                            [](auto value) { return value == 1; }))
        return Value{};
    return std::nullopt;
}
Value TextureInputs::gatherTexture(uint32_t stage, const Register &binding, const Register &sampler,
                                   uint32_t dimension, uint32_t returns, const Value &address,
                                   const GatherOperation &operation, const std::array<uint32_t, 4> &swizzle,
                                   uint32_t mask, const std::array<int32_t, 3> &offset, GpuOperations *gpu,
                                   bool &assisted) const {
    assisted = false;
    const bool comparison = operation.opcode == 126 || operation.opcode == 128;
    const bool programmable = operation.opcode == 127 || operation.opcode == 128;
    if ((operation.opcode != 109 && operation.opcode != 126 && !programmable) ||
        (dimension != 3 && dimension != 6 && dimension != 8 && dimension != 10) ||
        (programmable && dimension != 3 && dimension != 8) || operation.component > 3 ||
        (comparison && operation.component) || offset[2] || (programmable && (offset[0] || offset[1])))
        throw std::runtime_error("Invalid native gather operation");
    if (operation.precision[0] == 3 || operation.precision[0] > 5 || operation.precision[1] > 2 ||
        (operation.precision[2] && operation.precision[2] != 4 && operation.precision[2] != 5) ||
        operation.precision[3] > 2 || (stage == 5 && operation.outputDestination) ||
        std::any_of(operation.modifiers.begin(), operation.modifiers.end(),
                    [](auto value) { return value > 3; }) ||
        (!programmable && (operation.precision[2] || operation.modifiers[1] || operation.literal[1])) ||
        (!comparison && (operation.precision[3] || operation.modifiers[2] || operation.literal[2])))
        throw std::runtime_error("Invalid native gather precision or source controls");
    if (const auto resolved = prepareSample(stage, binding, sampler, dimension, returns, address,
                                            operation.reference, swizzle, mask, offset, {}, comparison, true))
        return *resolved;
    if (programmable && (operation.offset.valid & 3) != 3)
        return {};
    if (!gpu)
        throw std::runtime_error("Native gather requires the selected adapter GPU assist");
    const auto &view = views.at(binding);
    const auto type = loadReturnType(view.format);
    for (unsigned c = 0; c < 4; ++c) {
        const auto declared = (returns >> (4 * c)) & 15;
        if (declared != type && !(type == 5 && (declared == 1 || declared == 2)))
            throw std::runtime_error("Gather declaration and view return class disagree");
    }
    auto value = gpu->gatherTexture(stage, resources.at(*view.resource), view, samplers.at(sampler),
                                    dimension, returns, address, operation, swizzle, mask, offset);
    value.valid = uint8_t(mask);
    assisted = true;
    return value;
}
std::array<Value, 4> TextureInputs::samplePixelQuad(
    uint32_t opcode, const Register &binding, const Register &samplerBinding, uint32_t dimension,
    uint32_t returns, const std::array<PixelSamplingInvocation, 4> &invocations,
    const std::array<uint32_t, 4> &swizzle, uint32_t mask, const std::array<int32_t, 3> &offset,
    GpuOperations *gpu, bool &assisted, const std::array<uint32_t, 3> &precision) const {
    assisted = false;
    if (opcode != 69 && opcode != 70 && opcode != 74 && opcode != 108)
        throw std::runtime_error("Invalid implicit pixel sampling opcode");
    if (std::any_of(precision.begin(), precision.end(), [](auto p) { return p > 2; }) ||
        ((opcode == 69 || opcode == 108) && precision[2]) ||
        (opcode == 108 && std::any_of(precision.begin(), precision.end(), [](auto p) { return p != 0; })))
        throw std::runtime_error("Invalid implicit sampling floating-point precision");
    const uint32_t addressMask = opcode == 108                      ? (dimension == 2 || dimension == 7   ? 1
                                                                       : dimension == 3 || dimension == 8 ? 3
                                                                                                          : 7)
                                 : dimension == 2                   ? 1
                                 : dimension == 3 || dimension == 7 ? 3
                                 : dimension == 10                  ? 15
                                                                    : 7;
    const auto unknown = std::find_if(invocations.begin(), invocations.end(), [&](const auto &lane) {
        return (lane.address.valid & addressMask) != addressMask;
    });
    for (const auto &lane : invocations) {
        if (lane.address.valid > 15 || lane.parameter.valid > 15)
            throw std::runtime_error("Invalid implicit sampling input validity");
        if (opcode == 74 && (lane.parameter.valid & 1)) {
            const auto bias = std::bit_cast<float>(lane.parameter.bits[0]);
            if (!std::isfinite(bias) || bias < -16.f || bias > 15.99f)
                throw std::runtime_error("Implicit sampling bias is outside its defined range");
        }
    }
    // Unknown coordinates can affect every lane's implicit footprint. A missing
    // per-pixel comparison reference or bias does not define that lane's result.
    const auto &representative = unknown == invocations.end() ? invocations[0] : *unknown;
    if (const auto resolved =
            prepareSample(0, binding, samplerBinding, dimension, returns, representative.address,
                          Value::known({}), swizzle, mask, offset, {}, opcode == 70, false, opcode == 108))
        return {*resolved, *resolved, *resolved, *resolved};
    auto batch = invocations;
    for (auto &lane : batch)
        if (opcode == 69 || opcode == 108 || !(lane.parameter.valid & 1))
            lane.parameter = Value::known({});
    if (!gpu)
        throw std::runtime_error("Implicit pixel sampling requires the selected adapter GPU assist");
    const auto &view = views.at(binding);
    auto result =
        gpu->samplePixelQuad(opcode, resources.at(*view.resource), view, samplers.at(samplerBinding),
                             dimension, returns, batch, swizzle, mask, offset, precision);
    for (unsigned i = 0; i < 4; ++i)
        result[i].valid =
            opcode == 69 || opcode == 108 || (invocations[i].parameter.valid & 1) ? uint8_t(mask) : 0;
    assisted = true;
    return result;
}
Value TextureInputs::sampleTexture(uint32_t stage, const Register &binding, const Register &samplerBinding,
                                   uint32_t dimension, uint32_t returns, const Value &address,
                                   const Value &lodOrDdx, const std::array<uint32_t, 4> &swizzle,
                                   uint32_t mask, const std::array<int32_t, 3> &offset, GpuOperations *gpu,
                                   bool &assisted, const std::optional<Value> &ddy, bool comparison) const {
    assisted = false;
    if (const auto resolved = prepareSample(stage, binding, samplerBinding, dimension, returns, address,
                                            lodOrDdx, swizzle, mask, offset, ddy, comparison))
        return *resolved;
    const auto &view = views.at(binding);
    const auto &texture = resources.at(*view.resource);
    const auto sampler = samplers.find(samplerBinding);
    if (!gpu)
        throw std::runtime_error("Native texture sampling requires GPU assistance");
    auto result = gpu->sampleTexture(stage, texture, view, sampler->second, dimension, returns, address,
                                     lodOrDdx, swizzle, mask, offset, ddy, comparison);
    result.valid = uint8_t(mask);
    assisted = true;
    return result;
}
std::optional<std::vector<Value>> TextureInputs::sampleGradBatch(
    const Register &binding, const Register &samplerBinding, uint32_t dimension, uint32_t returns,
    const std::array<uint32_t, 3> &groupSize, const std::vector<SamplingInvocation> &invocations,
    const std::array<uint32_t, 4> &swizzle, uint32_t mask, const std::array<int32_t, 3> &offset,
    GpuOperations *gpu, bool &assisted) const {
    assisted = false;
    const uint64_t count = uint64_t(groupSize[0]) * groupSize[1] * groupSize[2];
    if (!groupSize[0] || !groupSize[1] || !groupSize[2] || groupSize[0] > 1024 || groupSize[1] > 1024 ||
        groupSize[2] > 64 || count > 1024 || count != invocations.size())
        throw std::runtime_error("Invalid native sampling thread group");
    std::vector<Value> result(invocations.size());
    const auto first =
        std::find_if(invocations.begin(), invocations.end(), [](const auto &v) { return v.active; });
    if (first == invocations.end())
        return result;
    const uint32_t addressMask = dimension == 2                     ? 1
                                 : dimension == 3 || dimension == 7 ? 3
                                 : dimension == 10                  ? 15
                                                                    : 7;
    const uint32_t gradientMask = dimension == 2 || dimension == 7   ? 1
                                  : dimension == 3 || dimension == 8 ? 3
                                                                     : 7;
    const auto unknown = std::find_if(invocations.begin(), invocations.end(), [&](const auto &lane) {
        return lane.active && ((lane.address.valid & addressMask) != addressMask ||
                               (lane.ddx.valid & gradientMask) != gradientMask ||
                               (lane.ddy.valid & gradientMask) != gradientMask);
    });
    const auto &representative = unknown == invocations.end() ? *first : *unknown;
    // Validate shared descriptors and potentially large storage once, not once
    // per lane. Only the selected operands vary within this request.
    const auto resolved =
        prepareSample(5, binding, samplerBinding, dimension, returns, representative.address,
                      representative.ddx, swizzle, mask, offset, representative.ddy);
    if (resolved) {
        if ((resolved->valid & mask) != mask)
            return std::nullopt;
        for (size_t i = 0; i < invocations.size(); ++i)
            if (invocations[i].active)
                result[i] = *resolved;
        return result;
    }
    if (!gpu)
        throw std::runtime_error("Native sampling group requires GPU assistance");
    const auto &view = views.at(binding);
    result = gpu->sampleGradBatch(resources.at(*view.resource), view, samplers.at(samplerBinding), dimension,
                                  returns, groupSize, invocations, swizzle, mask, offset);
    for (size_t i = 0; i < result.size(); ++i)
        result[i].valid = invocations[i].active ? uint8_t(mask) : 0;
    assisted = true;
    return result;
}
Value TextureInputs::loadMultisample(const Register &binding, uint32_t dimension, uint32_t returns,
                                     const Value &address, const Value &sample,
                                     const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                                     const std::array<int32_t, 3> &offset, bool &assisted) const {
    if ((dimension != 4 && dimension != 9) || binding.kind != 7 || mask > 15)
        throw std::runtime_error("Invalid multisample load dimension or binding");
    const uint32_t required = dimension == 4 ? 3 : 7;
    if ((address.valid & required) != required || !(sample.valid & 1))
        throw std::runtime_error("Unknown multisample load address or sample");
    for (auto component : swizzle)
        if (component > 3)
            throw std::runtime_error("Invalid multisample load swizzle");
    const auto selected = views.find(binding);
    if (selected == views.end())
        return {};
    const auto &view = selected->second;
    if (!view.resource)
        return Value::known({});
    if (view.dimension != (dimension == 4 ? 6u : 7u))
        throw std::runtime_error("Multisample declaration disagrees with its native view");
    if (!view.samples)
        return {};
    const auto &observed = *view.samples;
    const auto info = textureInfo(observed.resource);
    const auto count = uint64_t(info.width) * info.height * view.layerCount * info.samples;
    if (observed.resource.id != *view.resource || info.dimension != 3 || !info.samples || info.samples > 32 ||
        info.mips != 1 || view.firstMip || view.mipCount != 1 || !view.layerCount ||
        view.firstLayer >= info.layers || view.layerCount > info.layers - view.firstLayer ||
        observed.format != view.format || observed.dimension != view.dimension ||
        observed.firstLayer != view.firstLayer || observed.layerCount != view.layerCount ||
        observed.returnType != loadReturnType(view.format) || !observed.returnType ||
        observed.values.size() != count + 1 || view.nativeDescriptor.size() != 6 ||
        view.nativeDescriptor[0] != view.format || view.nativeDescriptor[1] != view.dimension ||
        (dimension == 9
             ? view.nativeDescriptor[2] != view.firstLayer || view.nativeDescriptor[3] != view.layerCount
             : view.firstLayer || view.layerCount != 1 || info.layers != 1))
        throw std::runtime_error("Invalid multisample observation or view range");
    for (unsigned c = 0; c < 4; ++c) {
        const auto type = (returns >> (c * 4)) & 15;
        if (type != observed.returnType && !(observed.returnType == 5 && (type == 1 || type == 2)))
            throw std::runtime_error("Multisample return class disagrees with its view");
    }
    // D3D11 functional specification 22.4.7 leaves an invalid sample index undefined.
    // Do not turn a particular driver's response into a defined zero or a wrapped sample.
    if (sample.bits[0] >= info.samples)
        return {};
    const auto x = address.bits[0] + uint32_t(offset[0]);
    const auto y = address.bits[1] + uint32_t(offset[1]);
    const auto layer = dimension == 9 ? address.bits[2] : 0;
    const auto index =
        x >= info.width || y >= info.height || layer >= view.layerCount
            ? count
            : ((uint64_t(layer) * info.height + y) * info.width + x) * info.samples + sample.bits[0];
    Value result;
    for (unsigned c = 0; c < 4; ++c)
        if (mask & (1u << c)) {
            result.bits[c] = observed.values.at(size_t(index))[swizzle[c]];
            result.valid |= uint8_t(1u << c);
        }
    assisted = true;
    return result;
}
Value TextureInputs::load(uint32_t stage, const Register &binding, uint32_t dimension, uint32_t returns,
                          const Value &address, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                          const std::array<int32_t, 3> &offset, GpuOperations *gpu, bool &assisted,
                          const std::array<uint32_t, 2> &precision, bool refactoringAllowed,
                          bool outputDestination, bool literalAddress) const {
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
    const auto format = loadFormat(view.format);
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
    auto effectiveAddress = address;
    auto result = gpu->loadTexture(stage, texture, view, dimension, returns, effectiveAddress, swizzle, mask,
                                   offset, precision, refactoringAllowed, outputDestination, literalAddress);
    assisted = true;
    const auto mip = uint64_t(view.firstMip) + effectiveAddress.bits[3];
    const auto layer = dimension == 7   ? effectiveAddress.bits[1]
                       : dimension == 8 ? effectiveAddress.bits[2]
                                        : 0;
    const auto x = effectiveAddress.bits[0] + uint32_t(offset[0]);
    const auto y = dimension == 3 || dimension == 5 || dimension == 8
                       ? effectiveAddress.bits[1] + uint32_t(offset[1])
                       : 0;
    const auto z = dimension == 5 ? effectiveAddress.bits[2] + uint32_t(offset[2]) : 0;
    if (effectiveAddress.bits[3] >= view.mipCount || layer >= view.layerCount ||
        texture.minLod > view.firstMip + view.mipCount - 1 || mip < std::floor(texture.minLod)) {
        result.valid = uint8_t(mask);
        return result;
    }
    const auto &sub = layout.at(size_t((view.firstLayer + layer) * info.mips + mip));
    const auto plane = textureInputPlane(texture.resource, view, uint32_t(mip), view.firstLayer + layer);
    if (x >= (plane ? plane->width : sub.width) || y >= (plane ? plane->height : sub.height) ||
        z >= sub.depth) {
        result.valid = uint8_t(mask);
        return result;
    }
    const auto start = (plane ? plane->offset : sub.offset) + uint64_t(z) * sub.slicePitch +
                       uint64_t(y / format->height) * sub.rowPitch +
                       uint64_t(x / format->width) * format->bytes;
    if (start > texture.storage.bytes.size() || format->bytes > texture.storage.bytes.size() - start)
        throw std::runtime_error("Native texture load format exceeds its storage layout");
    std::array<bool, 4> known{};
    for (unsigned c = 0; c < 4; ++c) {
        known[c] = true;
        for (unsigned byte = 0; byte < format->bytes; ++byte)
            if ((format->dependencies[c] & (1u << byte)) && !texture.storage.valid[start + byte])
                known[c] = false;
    }
    result.valid = 0;
    for (unsigned c = 0; c < 4; ++c)
        if ((mask & (1u << c)) && known[swizzle[c]])
            result.valid |= uint8_t(1u << c);
    return result;
}
} // namespace flora::dxbc_exec
