#include "TextureEdits.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace flora {
std::vector<TextureSubresource> textureSubresources(const Resource &resource) {
    const auto info = textureInfo(resource);
    if (!info.width || !info.height || !info.depth || !info.mips || info.mips > 32 || !info.layers ||
        uint64_t(info.mips) * info.layers > 30720 || !info.samples)
        throw std::runtime_error("Invalid explicit texture subresources");
    if (info.format >= 103 && info.format <= 105 &&
        (info.dimension != 3 || info.mips != 1 || info.samples != 1 || (resource.desc.back() & 4)))
        throw std::runtime_error(
            "Planar storage requires a non-MSAA 2D texture with one mip and no cube faces");
    std::vector<TextureSubresource> result;
    uint64_t offset = 0;
    for (uint32_t layer = 0; layer < info.layers; ++layer)
        for (uint32_t mip = 0; mip < info.mips; ++mip) {
            auto width = std::max(1u, info.width >> mip), height = std::max(1u, info.height >> mip),
                 depth = std::max(1u, info.depth >> mip);
            auto [pitch, rows] = pitches(width, height, info.format);
            auto slicePitch = uint64_t(pitch) * rows;
            if (slicePitch > (UINT64_MAX - offset) / depth)
                throw std::runtime_error("Texture storage size overflow");
            auto size = slicePitch * depth;
            result.push_back(
                {mip, layer, width, height, depth, pitch, slicePitch, offset, size, uint32_t(result.size())});
            offset += size;
        }
    return result;
}
MsaaEditEncoding msaaEditEncoding(const Resource &resource, std::optional<uint32_t> typedFormat) {
    const auto info = textureInfo(resource);
    const auto fmt = info.format;
    MsaaEditEncoding result;
    auto bind = resource.desc.at(resource.desc.size() - 3);
    if ((bind & 64) || fmt == 20 || fmt == 40 || fmt == 45 || fmt == 55) {
        switch (fmt) {
        case 19:
        case 20:
            result.depth = {{20, 19, 21, 22}};
            break;
        case 44:
        case 45:
            result.depth = {{45, 44, 46, 47}};
            break;
        case 39:
        case 40:
            result.depth = {{40, 39, 41, 0}};
            break;
        case 53:
        case 55:
            result.depth = {{55, 53, 56, 0}};
            break;
        }
    }
    struct Family {
        uint32_t first, last, access;
    };
    static constexpr Family families[] = {{1, 4, 3},    {5, 8, 7},    {9, 14, 12},  {15, 18, 17},
                                          {23, 25, 25}, {27, 32, 30}, {33, 38, 36}, {39, 43, 42},
                                          {48, 52, 50}, {53, 59, 57}, {60, 64, 62}};
    for (const auto &family : families)
        if (fmt >= family.first && fmt <= family.last) {
            if (typedFormat && (*typedFormat < family.first || *typedFormat > family.last))
                throw std::runtime_error("MSAA edit typed format is outside the resource family");
            result.storage = family.first;
            result.access = family.access;
            result.integerBits = true;
            result.display =
                result.depth ? (*result.depth)[0] : typedFormat.value_or(fmt == family.first ? fmt + 1 : fmt);
            // R32_TYPELESS defaults to R32_FLOAT rather than D32_FLOAT.
            if (!result.depth && !typedFormat && fmt == 39)
                result.display = 41;
            return result;
        }
    if (result.depth) {
        if (typedFormat &&
            std::find(result.depth->begin(), result.depth->end(), *typedFormat) == result.depth->end())
            throw std::runtime_error("MSAA depth edit typed format is outside the resource family");
        result.storage = (*result.depth)[1];
        result.access = result.display = (*result.depth)[0];
        return result;
    }
    const auto color = [&](uint32_t storage, uint32_t access,
                           std::initializer_list<uint32_t> members) -> std::optional<MsaaEditEncoding> {
        if (std::find(members.begin(), members.end(), fmt) == members.end())
            return {};
        if (typedFormat && std::find(members.begin(), members.end(), *typedFormat) == members.end())
            throw std::runtime_error("MSAA edit typed format is outside the resource family");
        return MsaaEditEncoding{
            storage, access, typedFormat.value_or(fmt == storage ? access : fmt), false, {}};
    };
    for (const auto &value : {color(90, 87, {87, 90, 91}), color(92, 88, {88, 92, 93}), color(26, 26, {26}),
                              color(85, 85, {85}), color(86, 86, {86}), color(115, 115, {115})})
        if (value)
            return *value;
    throw std::runtime_error("MSAA sample editing format is not implemented: " + std::to_string(fmt));
}
TextureSubresource validateTexturePatch(const Resource &resource, const TexturePatch &patch) {
    const auto info = textureInfo(resource);
    const auto subs = textureSubresources(resource);
    const auto found = std::find_if(subs.begin(), subs.end(), [&](const auto &sub) {
        return sub.mip == patch.mip && sub.layer == patch.layer;
    });
    if (found == subs.end())
        throw std::runtime_error("Texture edit mip/layer out of bounds");
    if (found->size != patch.bytes.size())
        throw std::runtime_error("Texture edit requires packed bytes of one complete subresource");
    if (info.samples == 1) {
        if (patch.sample || patch.typedFormat)
            throw std::runtime_error("Non-MSAA texture edits do not accept sample or typed format");
        return *found;
    }
    if (info.dimension != 3 || info.samples > 32 || info.mips != 1 || !patch.sample ||
        *patch.sample >= info.samples)
        throw std::runtime_error("Select an explicit sample of a 2D MSAA texture with one mip");
    const auto encoding = msaaEditEncoding(resource, patch.typedFormat);
    if (encoding.depth && (*encoding.depth)[0] == 20) {
        Reader data(patch.bytes);
        while (data.remaining()) {
            auto value = data.read<float>();
            if (!std::isfinite(value) || value < 0 || value > 1 ||
                (value != 0 && std::abs(value) < std::numeric_limits<float>::min()))
                throw std::runtime_error("D32S8 sample depth must be finite, non-subnormal and in [0,1]");
            data.skip(1);
            for (auto byte : data.take(3))
                if (byte)
                    throw std::runtime_error("D32S8 sample padding bytes must be zero");
        }
    }
    if (encoding.access == 26) {
        Reader data(patch.bytes);
        while (data.remaining()) {
            auto word = data.read<uint32_t>();
            for (auto [value, bits] :
                 {std::pair{word & 2047, 6u}, {(word >> 11) & 2047, 6u}, {word >> 22, 5u}})
                if ((value >> bits) == 31 && (value & ((1u << bits) - 1)))
                    throw std::runtime_error("R11G11B10 sample edits cannot preserve NaN payloads");
        }
    }
    if (encoding.access == 88)
        for (size_t offset = 3; offset < patch.bytes.size(); offset += 4)
            if (patch.bytes[offset])
                throw std::runtime_error("BGRX sample X bytes must be zero");
    return *found;
}
bool TextureOutputBinding::contains(uint32_t selectedMip, uint32_t layer) const {
    return selectedMip == mip && layer >= firstLayer && uint64_t(layer) < uint64_t(firstLayer) + layerCount;
}
namespace {
bool owns(const Frame &frame, Id view, Id resource) {
    if (!view)
        return false;
    Reader reader(frame.payload(view, 5));
    reader.skip(16);
    return reader.read<Id>() == resource;
}
} // namespace
std::vector<Id> textureInputViews(const Frame &frame, const Event &event, const State &state, Id resource) {
    std::set<Id> views;
    const bool compute = event.type == 0x35 || event.type == 0x36;
    for (unsigned stage = compute ? 5 : 0; stage < (compute ? 6u : 5u); ++stage)
        for (auto view : state.stages[stage].srv)
            if (owns(frame, view, resource))
                views.insert(view);
    return {views.begin(), views.end()};
}
std::vector<TextureOutputBinding> textureOutputBindings(const Frame &frame, const Event &event,
                                                        const State &state, Id resource) {
    const bool compute = event.type == 0x35 || event.type == 0x36;
    std::vector<TextureOutputBinding> out;
    const auto add = [&](Id view, const char *role, int slot) {
        if (!owns(frame, view, resource))
            return;
        Reader reader(frame.payload(view, 5));
        reader.skip(24);
        TextureOutputBinding binding;
        binding.role = role;
        binding.stage = compute ? "cs" : "om";
        binding.slot = slot;
        binding.view = view;
        binding.format = reader.read<uint32_t>();
        binding.dimension = reader.read<uint32_t>();
        auto dim = binding.dimension;
        if (binding.role == "dsv") {
            binding.dsvFlags = reader.read<uint32_t>();
            if (dim == 1 || dim == 3)
                binding.mip = reader.read<uint32_t>();
            else if (dim == 2 || dim == 4) {
                binding.mip = reader.read<uint32_t>();
                binding.firstLayer = reader.read<uint32_t>();
                binding.layerCount = reader.read<uint32_t>();
            } else if (dim == 6) {
                binding.firstLayer = reader.read<uint32_t>();
                binding.layerCount = reader.read<uint32_t>();
            } else if (dim != 5)
                throw std::runtime_error("Unsupported output DSV dimension");
        } else {
            if (dim == 2 || dim == 4)
                binding.mip = reader.read<uint32_t>();
            else if (dim == 3 || dim == 5) {
                binding.mip = reader.read<uint32_t>();
                binding.firstLayer = reader.read<uint32_t>();
                binding.layerCount = reader.read<uint32_t>();
            } else if (dim == 7 && binding.role == "rtv") {
                binding.firstLayer = reader.read<uint32_t>();
                binding.layerCount = reader.read<uint32_t>();
            } else if (dim == 8) {
                binding.mip = reader.read<uint32_t>();
                binding.depthSlices = reader.array<uint32_t, 2>();
            } else if (dim != 6 || binding.role != "rtv")
                throw std::runtime_error("Unsupported output texture view dimension");
        }
        out.push_back(binding);
    };
    const auto count = compute ? 64u : state.rtCount;
    if (count > 64)
        throw std::runtime_error("Output slot count invalid");
    for (uint32_t slot = 0; slot < count; ++slot) {
        auto view = compute ? (slot < 8 ? state.csUav[slot] : state.csExtended[slot - 8])
                            : (slot < 8 ? state.rtv[slot] : state.omExtended[slot - 8]);
        add(view, compute || slot >= state.omStart ? "uav" : "rtv", int(slot));
    }
    if (!compute)
        add(state.dsv, "dsv", -1);
    return out;
}
void validateTextureBinding(const Frame &frame, const Event &event, const State &state, Id resource,
                            const TexturePatch &patch, bool output) {
    if (!isDraw(event.type))
        throw std::runtime_error("Texture edits require a draw or dispatch event");
    if (output) {
        const auto bindings = textureOutputBindings(frame, event, state, resource);
        if (std::none_of(bindings.begin(), bindings.end(),
                         [&](const auto &binding) { return binding.contains(patch.mip, patch.layer); }))
            throw std::runtime_error("Texture subresource is not bound as an output of the selected event");
    } else if (textureInputViews(frame, event, state, resource).empty())
        throw std::runtime_error("Texture is not bound as an input of the selected event");
}
} // namespace flora
