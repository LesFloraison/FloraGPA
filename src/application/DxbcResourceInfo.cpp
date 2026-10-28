#include "DxbcGpuOperations.h"
#include "DxbcTextureInputs.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace flora::dxbc_exec {
Value TextureInputs::sampleQuery(const Register &binding, uint32_t dimension, bool position, bool integer,
                                 const Value &sample, bool &assisted) const {
    assisted = false;
    const SampleQueryInput *query = nullptr;
    if (binding.kind == 14) {
        if (!binding.indices.empty() || dimension)
            throw std::runtime_error("Invalid rasterizer sample query binding");
        query = rasterizerQueries.get();
    } else {
        if (binding.kind != 7 || binding.indices.size() != 1 || (dimension != 4 && dimension != 9))
            throw std::runtime_error("Invalid sample query resource binding");
        const auto selected = views.find(binding);
        if (selected == views.end())
            return {};
        const auto &view = selected->second;
        if (!view.resource)
            return Value::known({});
        if (view.dimension != (dimension == 4 ? 6u : 7u))
            throw std::runtime_error("Sample query dimension disagrees with its native view");
        query = view.queries.get();
    }
    if (!query)
        return {};
    if (!query->observed) {
        if (query->count || !query->positions.empty())
            throw std::runtime_error("Unobserved sample query contains values");
        Value result;
        result.valid = position ? 12 : 14;
        return result;
    }
    if (query->count > 32 || query->positions.size() != query->count)
        throw std::runtime_error("Invalid sample query observation");
    for (const auto &bits : query->positions)
        if (bits[2] || bits[3])
            throw std::runtime_error("Invalid sample position observation");
    Value result = Value::known({});
    if (!position)
        result.bits[0] = integer ? query->count : std::bit_cast<uint32_t>(float(query->count));
    else if (!(sample.valid & 1))
        result.valid = 12;
    else if (sample.bits[0] < query->count)
        result.bits = query->positions.at(sample.bits[0]);
    assisted = true;
    return result;
}
Value TextureInputs::dimensions(const Register &binding, uint32_t dimension, const Value &mip, uint32_t mode,
                                GpuOperations *gpu, bool &assisted) const {
    assisted = false;
    const bool uav = binding.kind == 30;
    const uint32_t native = dimension == 2            ? 2
                            : dimension == 3          ? 4
                            : dimension == 5          ? 8
                            : dimension == 7          ? 3
                            : dimension == 8          ? 5
                            : !uav && dimension == 4  ? 6
                            : !uav && dimension == 6  ? 9
                            : !uav && dimension == 9  ? 7
                            : !uav && dimension == 10 ? 10
                                                      : 0;
    if ((!uav && binding.kind != 7) || !native || mode > 2)
        throw std::runtime_error("Invalid DXBC resource information query");
    const auto selected = views.find(binding);
    if (selected == views.end())
        return {};
    const auto &view = selected->second;
    const uint8_t sizes = dimension == 2                                                         ? 1
                          : dimension == 3 || dimension == 4 || dimension == 6 || dimension == 7 ? 3
                                                                                                 : 7;
    const uint8_t spatial = dimension == 2 || dimension == 7 ? 1 : dimension == 5 ? 7 : 3;
    Value result = Value::known({});
    if (view.resource) {
        TextureMetadata description;
        if (auto found = metadata.find(*view.resource); found != metadata.end())
            description = found->second;
        else if (auto stored = resources.find(*view.resource); stored != resources.end())
            description = {stored->second.resource, stored->second.minLod};
        else
            return {};
        const auto info = textureInfo(description.resource);
        const auto expectedDimension = dimension == 2 || dimension == 7 ? 2u : dimension == 5 ? 4u : 3u;
        if (info.dimension != expectedDimension || !info.width || !info.height || !info.depth || !info.mips ||
            info.mips > 32 || !info.layers || !info.samples ||
            (dimension != 4 && dimension != 9 && info.samples != 1) || view.dimension != native ||
            view.nativeDescriptor.size() != (uav ? 5u : 6u) || view.nativeDescriptor[0] != view.format ||
            view.nativeDescriptor[1] != native || view.firstMip >= info.mips || !view.mipCount ||
            view.mipCount > info.mips - view.firstMip || view.firstLayer >= info.layers || !view.layerCount ||
            view.layerCount > info.layers - view.firstLayer || !std::isfinite(description.minLod) ||
            description.minLod < 0 || description.minLod > info.mips)
            throw std::runtime_error("Invalid native texture metadata for resource information");
        if (uav && ((info.format >= 103 && info.format <= 105) || (view.planeSlice && *view.planeSlice)))
            throw std::runtime_error("Planar UAV resource information requires plane dimensions");
        const auto plane =
            uav ? std::optional<TexturePlane>{}
                : textureInputPlane(description.resource, view, view.firstMip, view.firstLayer);
        const auto &d = view.nativeDescriptor;
        auto count = [](uint32_t raw, uint32_t first, uint32_t total) {
            return raw == UINT32_MAX ? total - first : raw;
        };
        const bool array = dimension == 7 || dimension == 8 || dimension == 9;
        if (uav) {
            if (view.mipCount != 1 || d[2] != view.firstMip ||
                (array &&
                 (d[3] != view.firstLayer || count(d[4], view.firstLayer, info.layers) != view.layerCount)))
                throw std::runtime_error("UAV resource information view range mismatch");
        } else if (dimension == 4 || dimension == 9) {
            if (view.firstMip || view.mipCount != 1 ||
                (array &&
                 (d[2] != view.firstLayer || count(d[3], view.firstLayer, info.layers) != view.layerCount)))
                throw std::runtime_error("Multisample resource information view range mismatch");
        } else if (d[2] != view.firstMip || count(d[3], view.firstMip, info.mips) != view.mipCount ||
                   (array && (d[4] != view.firstLayer ||
                              count(d[5], view.firstLayer, info.layers) != view.layerCount)) ||
                   (dimension == 10 && (d[4] != view.firstLayer || uint64_t(d[5]) * 6 != view.layerCount)))
            throw std::runtime_error("SRV resource information view range mismatch");
        if ((!array && dimension != 10 &&
             (view.firstLayer || view.layerCount != (dimension == 6 ? 6u : 1u))) ||
            ((dimension == 6 || dimension == 10) &&
             (!(description.resource.desc.back() & 4) || view.firstLayer % 6 || view.layerCount % 6)))
            throw std::runtime_error("Resource information layer identity mismatch");
        if (uav && dimension == 5) {
            const auto depth = std::max(1u, info.depth >> view.firstMip);
            if (!view.depthSlices || (*view.depthSlices)[0] >= depth || !(*view.depthSlices)[1] ||
                (*view.depthSlices)[1] > depth - (*view.depthSlices)[0] || d[3] != (*view.depthSlices)[0] ||
                count(d[4], d[3], depth) != (*view.depthSlices)[1])
                throw std::runtime_error("Resource information depth view mismatch");
        }
        result.bits[3] = view.mipCount;
        if (!(mip.valid & 1))
            result.valid &= uint8_t(~sizes);
        else if (mip.bits[0] < view.mipCount) {
            const auto level = view.firstMip + mip.bits[0];
            result.bits[0] = plane ? plane->width : std::max(1u, info.width >> level);
            if (sizes & 2)
                result.bits[1] = dimension == 7 ? view.layerCount
                                 : plane        ? plane->height
                                                : std::max(1u, info.height >> level);
            if (sizes & 4)
                result.bits[2] = dimension == 5 ? std::max(1u, info.depth >> level) : view.layerCount;
            if (!uav && (level < std::floor(description.minLod) ||
                         description.minLod > view.firstMip + view.mipCount - 1))
                result.valid &= uint8_t(~sizes);
        }
        if (dimension == 10) {
            result.bits[2] = 0;
            result.valid =
                uint8_t(result.valid & 11u); // Cube-array z is undefined by the instruction contract.
        }
        if ((mip.valid & 1) && (mode == 1 || mip.bits[0] >= view.mipCount || (uav && dimension == 5))) {
            if (!gpu)
                throw std::runtime_error("This resource query requires the selected GPU");
            auto observed =
                gpu->textureDimensions(description, view, binding.kind, dimension, mip.bits[0], mode);
            observed.valid = result.valid;
            assisted = true;
            return observed;
        }
    }
    if (mode != 2)
        for (unsigned c = 0; c < 4; ++c) {
            const auto number = float(result.bits[c]);
            const auto value = mode == 1 && (spatial & (1u << c))
                                   ? (number ? 1.f / number : std::numeric_limits<float>::infinity())
                                   : number;
            result.bits[c] = std::bit_cast<uint32_t>(value);
        }
    return result;
}
} // namespace flora::dxbc_exec
