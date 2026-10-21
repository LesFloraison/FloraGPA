#include "DrawResources.h"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <tuple>

namespace flora {
namespace {
uint64_t identity(const nlohmann::json &j, bool allowZero = false) {
    if (!j.is_string()) throw std::runtime_error("DX12 identity must be a decimal string");
    const auto &s = j.get_ref<const std::string &>();
    uint64_t v{};
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (error != std::errc{} || end != s.data() + s.size() || (!v && !allowZero))
        throw std::runtime_error("Invalid DX12 resource identity");
    return v;
}
uint32_t integer(const nlohmann::json &j) {
    if (!j.is_number_integer() || (!j.is_number_unsigned() && j.get<int64_t>() < 0) ||
        j.get<uint64_t>() > UINT32_MAX) throw std::runtime_error("Invalid unsigned texture coordinate");
    return j.get<uint32_t>();
}
}
uint32_t Dx12TextureRange::count() const {
    const uint64_t n = uint64_t(mips) * layers * planes;
    if (!n || n > INT_MAX) throw std::runtime_error("Invalid DX12 subresource count");
    return uint32_t(n);
}
uint32_t Dx12TextureRange::index(uint32_t mip, uint32_t layer, uint32_t plane) const {
    count();
    if (mip >= mips || layer >= layers || plane >= planes)
        throw std::runtime_error("DX12 subresource selection exceeds texture");
    return mip + mips * (layer + layers * plane);
}
Dx12TextureRange dx12TextureRange(const nlohmann::json &d) {
    const auto dimension = integer(d.at("Dimension"));
    if (dimension < 2 || dimension > 4) throw std::runtime_error("Selected resource is not a texture");
    Dx12TextureRange r;
    r.mips = integer(d.at("MipLevels"));
    r.layers = dimension == 4 ? 1 : integer(d.at("DepthOrArraySize"));
    r.depth = dimension == 4 ? integer(d.at("DepthOrArraySize")) : 1;
    r.format = integer(d.at("Format"));
    r.samples = integer(d.at("SampleCount"));
    r.planes = (r.format >= 19 && r.format <= 22) || (r.format >= 44 && r.format <= 47) ||
               r.format == 103 || r.format == 104 || r.format == 105 ? 2 : 1;
    uint32_t maximumMips{};
    {
        uint64_t extent = std::max({identity(d.at("Width")), uint64_t(integer(d.at("Height"))),
                                    uint64_t(r.depth)});
        do { ++maximumMips; extent >>= 1; } while (extent);
    }
    if (!r.mips) r.mips = maximumMips;
    if (!r.depth || !r.samples || r.samples > 32 || r.mips > 32 || r.mips > maximumMips ||
        !integer(d.at("Height")) || (r.samples > 1 && (r.mips != 1 || dimension != 3)))
        throw std::runtime_error("Invalid DX12 texture dimensions");
    r.count();
    return r;
}
void validateDx12ResourceRequest(const nlohmann::json &request) {
    identity(request.at("creation")); identity(request.at("object"));
    const auto &d = request.at("resource_description");
    const bool buffer = request.at("buffer").get<bool>();
    request.at("thumbnail").get<bool>();
    if (buffer != (integer(d.at("Dimension")) == 1))
        throw std::runtime_error("Resource kind does not match its captured description");
    if (buffer) {
        const auto width = identity(d.at("Width"));
        const auto offset = identity(request.at("offset"), true), length = identity(request.at("length"));
        if (length > 256ULL * 1024 * 1024 || offset > width || length > width - offset)
            throw std::runtime_error("Buffer range exceeds resource or 256 MiB limit");
        if (request.contains("view_buffer_range")) {
            const auto &view = request.at("view_buffer_range");
            const auto first = identity(view.at("offset"), true), size = identity(view.at("length"));
            if (first > width || size > width - first || offset < first || offset - first > size ||
                length > size - (offset - first)) throw std::runtime_error("Buffer range exceeds bound view");
        }
        return;
    }
    const auto r = dx12TextureRange(d);
    const auto mip = integer(request.at("mip")), layer = integer(request.at("layer"));
    const auto slice = integer(request.at("slice")), sample = integer(request.at("sample"));
    const auto plane = request.at("plane").get<std::string>();
    if (plane != "color" && plane != "depth" && plane != "stencil")
        throw std::runtime_error("Unknown texture plane");
    const bool stencil = (r.format >= 19 && r.format <= 22) || (r.format >= 44 && r.format <= 47);
    if (plane == "stencil" && !stencil) throw std::runtime_error("Texture has no stencil plane");
    // Multi-planar video needs a format conversion not provided by this viewer.
    if (r.format == 103 || r.format == 104 || r.format == 105)
        throw std::runtime_error("Planar video texture display is unavailable");
    r.index(mip, layer, plane == "stencil" ? 1u : 0u);
    const auto width = identity(d.at("Width"));
    if (width > UINT32_MAX) throw std::runtime_error("Texture width exceeds supported dimensions");
    const uint64_t pixels = std::max(uint64_t(1), width >> mip) * std::max(1u, integer(d.at("Height")) >> mip);
    if (pixels > (512ULL * 1024 * 1024) / 16 / std::max(1u, r.depth >> mip))
        throw std::runtime_error("Selected texture exceeds the 512 MiB display budget");
    if (slice >= std::max(1u, r.depth >> mip) || (sample != UINT32_MAX && sample >= r.samples))
        throw std::runtime_error("Texture slice or sample is out of range");
    if (request.contains("view_range")) {
        const auto &view = request.at("view_range");
        for (const auto &[axis, value, limit] : {
                 std::tuple{"mip", mip, r.mips}, std::tuple{"layer", layer, r.layers},
                 std::tuple{"slice", slice, std::max(1u, r.depth >> mip)}}) {
            const auto first = integer(view.at(std::string(axis) + "_first"));
            const auto end = integer(view.at(std::string(axis) + "_end"));
            if (first >= end || end > limit || value < first || value >= end)
                throw std::runtime_error("Texture selection exceeds bound view");
        }
    }
    const auto low = request.at("low").get<double>(), high = request.at("high").get<double>();
    if (!std::isfinite(low) || !std::isfinite(high) || high <= low)
        throw std::runtime_error("Invalid texture display range");
    const auto channel = request.at("channel").get<std::string>();
    if (channel != "RGB" && channel != "RGBA" && channel != "R" && channel != "G" && channel != "B" && channel != "A")
        throw std::runtime_error("Invalid texture channel");
    if (request.contains("component_type") && integer(request.at("component_type")) > 9)
        throw std::runtime_error("Invalid texture component type");
}
}
