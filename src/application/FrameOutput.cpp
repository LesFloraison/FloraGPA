#include "FrameOutput.h"
#include "Geometry.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace flora {
using Json = nlohmann::json;
namespace {
uint32_t typedFormat(uint32_t f) {
    static const std::map<uint32_t, uint32_t> types{
        {1, 2},   {5, 6},   {9, 10},  {15, 16}, {19, 21}, {23, 24}, {27, 28}, {33, 34},
        {39, 41}, {44, 46}, {48, 49}, {53, 54}, {60, 61}, {70, 71}, {73, 74}, {76, 77},
        {79, 80}, {82, 83}, {90, 87}, {92, 88}, {94, 95}, {97, 98}};
    auto it = types.find(f);
    return it == types.end() ? f : it->second;
}
double scalar(const Json &value) {
    if (value.is_number())
        return value.get<double>();
    if (value == "nan")
        return std::numeric_limits<double>::quiet_NaN();
    if (value == "inf")
        return std::numeric_limits<double>::infinity();
    if (value == "-inf")
        return -std::numeric_limits<double>::infinity();
    throw std::runtime_error("Invalid output scalar");
}
uint8_t quantize(double value, double low, double high) {
    if (std::isnan(value) || value <= low)
        return 0;
    if (value >= high)
        return 255;
    const auto scaled = (value - low) / (high - low) * 255;
    const auto floor = std::floor(scaled);
    const auto fraction = scaled - floor;
    return uint8_t(floor + (fraction > .5 || (fraction == .5 && (uint32_t(floor) & 1))));
}
} // namespace
std::vector<uint8_t> displayOutput(Bytes data, uint32_t format, const FrameDisplayOptions &options,
                                   const std::string &aspect) {
    const auto &channel = options.channel;
    const auto low = options.low, high = options.high;
    if ((channel != "rgba" && channel != "rgb" && channel != "r" && channel != "g" && channel != "b" &&
         channel != "a") ||
        !std::isfinite(low) || !std::isfinite(high) || !std::isfinite(high - low) || high <= low)
        throw std::runtime_error("Invalid frame display channel or finite increasing range");
    if (format == 29)
        format = 28;
    if (format == 91)
        format = 87;
    if (format == 93)
        format = 88;
    const auto size = pitches(1, 1, format).first;
    if (!aspect.empty() && ((aspect != "depth" && aspect != "stencil") ||
                            (format != 20 && format != 40 && format != 45 && format != 55) ||
                            (aspect == "stencil" && format != 20 && format != 45)))
        throw std::runtime_error("Selected depth/stencil aspect is not present in this format");
    if (data.size() % size)
        throw std::runtime_error("Output storage does not contain whole pixels");
    if (aspect.empty() && (channel == "rgba" || channel == "rgb") && low == 0 && high == 1) {
        if (format == 28 || format == 87 || format == 88) {
            std::vector<uint8_t> out(data.begin(), data.end());
            for (size_t i = 0; i < out.size(); i += 4) {
                if (format != 28)
                    std::swap(out[i], out[i + 2]);
                if (format == 88 || channel == "rgb")
                    out[i + 3] = 255;
            }
            return out;
        }
    }
    if (aspect.empty())
        vertexValue(std::vector<uint8_t>(size), 0, format);
    if (data.size() / size > std::vector<uint8_t>().max_size() / 4)
        throw std::runtime_error("Output display size overflow");
    std::vector<uint8_t> out(data.size() / size * 4);
    for (size_t offset = 0, n = 0; offset < data.size(); offset += size, n += 4) {
        if (!aspect.empty()) {
            Reader r(data.subspan(offset, size));
            double value;
            if (aspect == "stencil")
                value = data[offset + (format == 20 ? 4 : 3)];
            else if (format == 20 || format == 40)
                value = r.read<float>();
            else if (format == 45)
                value = (r.read<uint32_t>() & 0xffffff) / double(0xffffff);
            else
                value = r.read<uint16_t>() / 65535.;
            const auto q = channel == "a" ? uint8_t(255) : quantize(value, low, high);
            out[n] = out[n + 1] = out[n + 2] = q;
            out[n + 3] = 255;
            continue;
        }
        auto values = vertexValue(data, offset, format);
        std::array<double, 4> pixel{0, 0, 0, 1};
        for (size_t i = 0; i < values.size(); ++i)
            pixel[i] = scalar(values[i]);
        std::array<uint8_t, 4> q{};
        for (unsigned i = 0; i < 4; ++i)
            q[i] = quantize(pixel[i], low, high);
        if (channel == "rgba" || channel == "rgb") {
            std::copy(q.begin(), q.end(), out.begin() + ptrdiff_t(n));
            if (channel == "rgb")
                out[n + 3] = 255;
        } else {
            const auto component = std::string("rgba").find(channel);
            out[n] = out[n + 1] = out[n + 2] = q[component];
            out[n + 3] = 255;
        }
    }
    return out;
}
Json outputSubresource(const Resource &resource, Bytes descriptor, std::optional<uint32_t> layer,
                       bool depth) {
    const auto &desc = resource.desc;
    if (desc.size() == 6) {
        if (depth || layer)
            throw std::runtime_error(
                "Buffer RTV coverage has no depth view or array layer; leave layer empty");
        Reader r(descriptor);
        const auto format = r.read<uint32_t>(), dimension = r.read<uint32_t>(), first = r.read<uint32_t>(),
                   count = r.read<uint32_t>();
        r.skip(4);
        r.end();
        const auto [size, rows] = pitches(1, 1, format);
        if (dimension != 1 || !count || rows != 1 || (uint64_t(first) + count) * size > desc[0])
            throw std::runtime_error("Invalid buffer RTV element range");
        return {{"format", format},
                {"dimension", 1},
                {"mip", 0},
                {"layer", 0},
                {"relative_layer", 0},
                {"first_layer", 0},
                {"layer_count", 1},
                {"width", count},
                {"height", 1},
                {"resource_dimension", 1},
                {"layer_kind", "none"},
                {"first_element", first},
                {"element_count", count},
                {"element_size", size},
                {"byte_offset", uint64_t(first) * size},
                {"byte_length", uint64_t(count) * size}};
    }
    const auto info = textureInfo(resource);
    Reader r(descriptor);
    const auto format = r.read<uint32_t>();
    auto dimension = r.read<uint32_t>();
    uint32_t flags = 0;
    if (depth) {
        flags = r.read<uint32_t>();
        if (dimension < 1 || dimension > 6)
            throw std::runtime_error("Unsupported depth texture view");
        ++dimension;
    }
    const auto a = r.read<uint32_t>(), b = r.read<uint32_t>(), c = r.read<uint32_t>();
    r.end();
    uint32_t mip = 0, first = 0, count = 1;
    switch (dimension) {
    case 2:
    case 4:
        mip = a;
        break;
    case 3:
    case 5:
    case 8:
        mip = a;
        first = b;
        count = c;
        break;
    case 6:
        break;
    case 7:
        first = a;
        count = b;
        break;
    default:
        throw std::runtime_error("Unsupported texture RTV dimension");
    }
    if (!(resource.type == 0x84   ? (dimension == 2 || dimension == 3)
          : resource.type == 0x85 ? (dimension >= 4 && dimension <= 7)
                                  : resource.type == 0x86 && dimension == 8))
        throw std::runtime_error("RTV resource dimension mismatch");
    if (mip >= info.mips || mip >= 32)
        throw std::runtime_error("Invalid RTV mip");
    const auto limit = resource.type == 0x86 ? std::max(1u, info.depth >> mip) : info.layers;
    if (dimension == 8 && count == UINT32_MAX) {
        if (first >= limit)
            throw std::runtime_error("Invalid RTV subresource range");
        count = limit - first;
    }
    if (!count || uint64_t(first) + count > limit)
        throw std::runtime_error("Invalid RTV subresource range");
    if ((info.samples > 1) != (dimension == 6 || dimension == 7))
        throw std::runtime_error("RTV sampling dimension mismatch");
    const auto selected = layer.value_or(first);
    if (selected < first || uint64_t(selected) >= uint64_t(first) + count)
        throw std::runtime_error("Output layer must be within the view's absolute layer range");
    return {{"format", format},
            {"dimension", dimension},
            {"source_view", depth ? "dsv" : "rtv"},
            {"dsv_flags", flags},
            {"mip", mip},
            {"first_layer", first},
            {"layer_count", count},
            {"layer", selected},
            {"relative_layer", selected - first},
            {"width", std::max(1u, info.width >> mip)},
            {"height", std::max(1u, info.height >> mip)},
            {"resource_dimension", info.dimension},
            {"layer_kind", resource.type == 0x86 ? "w_slice" : "array_layer"},
            {"array_layer", resource.type == 0x86 ? 0 : selected},
            {"slice", resource.type == 0x86 ? selected : 0}};
}
FrameOutput readFrameOutput(Replay &replay, Id id, const FrameDisplayOptions &options, std::optional<Id> view,
                            const std::string &requestedAspect) {
    const auto &frame = replay.frame();
    const auto resource = frame.resource(id);
    uint32_t mip = options.mip.value_or(0), slice = 0, layer = options.layer.value_or(0), format = 0;
    uint32_t samples = 1, sourceFormat = 0, width = 0, height = 0;
    std::string kind, aspect = requestedAspect;
    Json selection, flags;
    const auto viewId = view.value_or(replay.lastOutputResource() == id ? replay.lastOutputView() : 0);
    if (viewId) {
        const auto &entry = frame.entry(viewId);
        if (entry.category != 5 || (entry.type != 0x8d && entry.type != 0x8e))
            throw std::runtime_error("Output view must be a captured RTV or DSV");
        Reader r(frame.payload(viewId));
        r.skip(16);
        if (r.read<Id>() != id)
            throw std::runtime_error("Output view does not belong to the selected resource");
        kind = entry.type == 0x8e ? "dsv" : "rtv";
        selection = outputSubresource(resource, r.take(r.remaining()), options.layer, kind == "dsv");
        format = selection["format"].get<uint32_t>();
        mip = selection["mip"].get<uint32_t>();
        layer = selection.value("array_layer", 0u);
        slice = selection.value("slice", 0u);
        if (kind == "dsv") {
            flags = selection["dsv_flags"];
            if (aspect.empty())
                aspect = "depth";
        }
    }
    if (!aspect.empty() && kind != "dsv")
        throw std::runtime_error("Depth/stencil output requires a DSV");
    FrameOutput result;
    if (resource.type == 0x83) {
        if (selection.is_null() || selection["dimension"] != 1 || !format)
            throw std::runtime_error("Buffer frame output needs a typed buffer RTV");
        if (options.sample)
            throw std::runtime_error("Buffer RTV output has no sample index");
        const auto offset = selection["byte_offset"].get<uint64_t>(),
                   size = selection["byte_length"].get<uint64_t>();
        auto storage = replay.readBuffer(id);
        if (offset > storage.size() || size > storage.size() - offset)
            throw std::runtime_error("Buffer RTV output range exceeds storage");
        result.storage.assign(storage.begin() + ptrdiff_t(offset),
                              storage.begin() + ptrdiff_t(offset + size));
        width = selection["element_count"].get<uint32_t>();
        height = 1;
        sourceFormat = format;
    } else {
        auto info = textureInfo(resource);
        sourceFormat = info.format;
        samples = info.samples;
        if (!format)
            format = typedFormat(sourceFormat);
        if (selection.is_null() && info.dimension == 4) {
            slice = layer;
            layer = 0;
        }
        if (mip >= info.mips || mip >= 32 || layer >= info.layers || slice >= std::max(1u, info.depth >> mip))
            throw std::runtime_error("Output layer or slice exceeds texture storage");
        if (options.sample && *options.sample >= samples)
            throw std::runtime_error("Output sample index exceeds source sample count");
        std::vector<uint8_t> storage;
        if (samples > 1) {
            auto resolved = replay.readMsaa(id, options.sample, format);
            info = textureInfo(resolved.resource);
            storage = std::move(resolved.bytes);
            result.msaa = {
                {"mode", resolved.mode},
                {"source_samples", samples},
                {"source_format", sourceFormat},
                {"output_format", info.format},
                {"selected_sample", options.sample ? Json(*options.sample) : Json(nullptr)},
                {"depth_stencil", resolved.depthStencil},
                {"initial_samples_reconstructed", false},
                {"initialization_note",
                 "Pre-capture per-sample contents are not reconstructed. Regions not written by recorded "
                 "commands or explicit experiment assets may differ from the captured application."}};
            if (resolved.integerBits)
                result.msaa["storage_path"] = "integer_bits";
            if (resolved.canonicalX)
                result.msaa["unused_x_byte"] = 0;
        } else
            storage = replay.readTexture(id);
        uint64_t offset = 0, selectedOffset = 0, selectedSize = 0;
        for (uint32_t l = 0; l < info.layers; ++l)
            for (uint32_t m = 0; m < info.mips; ++m) {
                const auto w = std::max(1u, info.width >> m), h = std::max(1u, info.height >> m);
                const auto [pitch, rows] = pitches(w, h, info.format);
                const auto sliceSize = uint64_t(pitch) * rows;
                const auto size = sliceSize * std::max(1u, info.depth >> m);
                if (offset > storage.size() || size > storage.size() - offset)
                    throw std::runtime_error("Output subresource exceeds storage");
                if (l == layer && m == mip) {
                    selectedOffset = offset + slice * sliceSize;
                    selectedSize = sliceSize;
                    width = w;
                    height = h;
                }
                offset += size;
            }
        result.storage.assign(storage.begin() + ptrdiff_t(selectedOffset),
                              storage.begin() + ptrdiff_t(selectedOffset + selectedSize));
    }
    auto pixels = displayOutput(result.storage, format, options, aspect);
    if (pixels.size() != uint64_t(width) * height * 4)
        throw std::runtime_error("Output display dimensions disagree with storage");
    result.image = {width, height, 28, std::move(pixels), id};
    result.display = {{"resource", id},
                      {"view", viewId ? Json(viewId) : Json(nullptr)},
                      {"view_kind", kind.empty() ? Json(nullptr) : Json(kind)},
                      {"aspect", aspect.empty() ? "color" : aspect},
                      {"dsv_flags", flags},
                      {"source_format", sourceFormat},
                      {"typed_format", format},
                      {"mip", mip},
                      {"layer", layer},
                      {"slice", slice},
                      {"sample", options.sample ? Json(*options.sample) : Json(nullptr)},
                      {"source_samples", samples},
                      {"view_selection", selection},
                      {"width", width},
                      {"height", height},
                      {"channel", options.channel},
                      {"range", {options.low, options.high}},
                      {"display_format", "RGBA8_UNORM"},
                      {"conversion", "clamp_and_round_storage_values"},
                      {"srgb", "encoded_values_preserved"},
                      {"hdr_tone_mapping", false},
                      {"nonfinite", "NaN and negative infinity map to 0; positive infinity maps to 255"},
                      {"storage_bytes", result.storage.size()},
                      {"storage_sha256", sha256(result.storage)},
                      {"storage_scope", samples > 1 ? (options.sample ? "selected_sample_subresource"
                                                                      : "selected_resolved_subresource")
                                                    : "selected_native_subresource"}};
    return result;
}
} // namespace flora
