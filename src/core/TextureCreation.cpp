#include "TextureCreation.h"
#include "TextureEdits.h"
namespace flora {
bool isTextureCreation(uint16_t t) { return t >= 0x3579 && t <= 0x357f; }
bool isViewCreation(uint16_t t) { return t >= 0x357c && t <= 0x357f; }
uint16_t viewCreationResourceType(uint16_t t) {
    switch (t) {
    case 0x357c:
        return 0x8c;
    case 0x357d:
        return 0x8f;
    case 0x357e:
        return 0x8d;
    case 0x357f:
        return 0x8e;
    default:
        throw std::runtime_error("Not a recovered view creation");
    }
}
unsigned viewCreationDescriptorWords(uint16_t t) {
    viewCreationResourceType(t);
    return t == 0x357c || t == 0x357f ? 6 : 5;
}
bool createdViewDescriptorEqual(uint16_t t, std::span<const uint32_t> a, std::span<const uint32_t> b) {
    if (t == 0x357c)
        return textureSrvDescriptorEqual(a, b);
    const auto count = viewCreationDescriptorWords(t);
    if (a.size() != count || b.size() != count || a[0] != b[0] || a[1] != b[1])
        return false;
    unsigned active = 0;
    if (t == 0x357d) {
        constexpr unsigned sizes[]{0, 5, 3, 5, 3, 5, 0, 0, 5};
        if (a[1] < std::size(sizes))
            active = sizes[a[1]];
    } else if (t == 0x357e) {
        constexpr unsigned sizes[]{0, 4, 3, 5, 3, 5, 2, 4, 5};
        if (a[1] < std::size(sizes))
            active = sizes[a[1]];
    } else {
        constexpr unsigned sizes[]{0, 4, 6, 4, 6, 3, 5};
        if (a[1] < std::size(sizes))
            active = sizes[a[1]];
    }
    return active && std::equal(a.begin(), a.begin() + active, b.begin());
}
ViewObservation viewCreationObservation(uint16_t t) {
    switch (t) {
    case 0x3026:
    case 0x3053:
    case 0x301d:
    case 0x3185:
        return ViewObservation::QueryInterface;
    case 0x3027:
    case 0x3028:
    case 0x3054:
    case 0x3055:
    case 0x301e:
    case 0x301f:
    case 0x3186:
    case 0x3187:
        return ViewObservation::ReferenceCount;
    case 0x3029:
    case 0x3056:
    case 0x3020:
    case 0x3188:
        return ViewObservation::GetDevice;
    case 0x302d:
    case 0x305a:
    case 0x3024:
    case 0x318c:
        return ViewObservation::GetResource;
    case 0x305b:
    case 0x3025:
    case 0x318d:
        return ViewObservation::GetDescriptor;
    default:
        return ViewObservation::None;
    }
}
bool textureSrvDescriptorEqual(std::span<const uint32_t> a, std::span<const uint32_t> b) {
    if (a.size() != 6 || b.size() != 6 || a[0] != b[0] || a[1] != b[1])
        return false;
    // Compare the active union only; padding and inactive descriptor members are not semantics.
    const auto words = a[1] == 11                                                      ? 5
                       : a[1] == 1                                                     ? 4
                       : a[1] == 2 || a[1] == 4 || a[1] == 7 || a[1] == 8 || a[1] == 9 ? 4
                       : a[1] == 3 || a[1] == 5 || a[1] == 10                          ? 6
                       : a[1] == 6                                                     ? 2
                                                                                       : 0;
    return words && std::equal(a.begin(), a.begin() + words, b.begin());
}
TextureCreationRecord readTextureCreation(uint16_t t, Bytes bytes) {
    if (!isTextureCreation(t))
        throw std::runtime_error("Not a recovered texture/view creation");
    Reader r(bytes);
    TextureCreationRecord out;
    out.type = t;
    out.link = r.read<Id>();
    out.device = r.read<Id>();
    out.result = r.read<int32_t>();
    if (isViewCreation(t))
        out.source = r.read<Id>();
    out.hasDescriptor = r.flag();
    const auto fields = t == 0x3579   ? 8u
                        : t == 0x357a ? 11u
                        : t == 0x357b ? 9u
                                      : viewCreationDescriptorWords(t);
    if (out.hasDescriptor)
        for (unsigned i = 0; i < fields; ++i)
            out.descriptor.push_back(r.read<uint32_t>());
    if (!isViewCreation(t)) {
        out.hasInitial = r.flag();
        if (out.hasInitial) {
            if (!out.hasDescriptor)
                throw std::runtime_error("Texture initial observations require a descriptor");
            const auto mips = out.descriptor[t == 0x3579 ? 1 : t == 0x357a ? 2 : 3];
            if (!mips || mips > 32)
                throw std::runtime_error("Invalid captured texture initial observation count");
            // Original shim serializes MipLevels observations, not MipLevels*ArraySize.
            for (uint32_t i = 0; i < mips; ++i)
                out.initial.push_back({r.read<Id>(), r.read<uint32_t>(), r.read<uint32_t>()});
        }
    }
    out.resource = r.read<Id>();
    r.end();
    return out;
}
TextureCreationAudit auditTextureCreations(const Frame &frame) {
    TextureCreationAudit audit;
    for (const auto &[id, e] : frame.entries()) {
        if (e.category != 7 || !isTextureCreation(e.type))
            continue;
        auto &c = audit.records[id];
        try {
            c = readTextureCreation(e.type, frame.payload(id));
            if (c.link)
                throw std::runtime_error("Linked texture/view creation is unresolved");
            if (c.result != 0) {
                if (c.result > 1 || c.resource)
                    throw std::runtime_error("Unexpected creation result or returned identity");
                continue; // Failed calls / validation-only S_FALSE do not create a resource.
            }
            if (!c.resource)
                throw std::runtime_error("Successful texture/view creation has no returned identity");
            const auto [it, inserted] = audit.creationEvents.emplace(c.resource, id);
            if (!inserted) {
                audit.records.at(it->second).error =
                    "Repeated creation identity requires resource versioning";
                throw std::runtime_error("Repeated creation identity requires resource versioning");
            }
            frame.payload(c.device, 5, 0x81);
            const auto resource = frame.resource(c.resource);
            if (isViewCreation(e.type)) {
                Reader r(frame.capturedPayload(c.resource, 5, viewCreationResourceType(e.type)));
                r.skip(16);
                if (r.read<Id>() != c.source)
                    throw std::runtime_error("Created view source differs from its saved resource");
                std::vector<uint32_t> saved;
                for (unsigned i = 0; i < viewCreationDescriptorWords(e.type); ++i)
                    saved.push_back(r.read<uint32_t>());
                r.end();
                const auto source = frame.resource(c.source);
                if (resource.device != c.device || source.device != c.device || source.type < 0x83 ||
                    source.type > 0x86)
                    throw std::runtime_error("Created view source kind or device is inconsistent");
                if (source.type == 0x83) {
                    if (e.type == 0x357f || (saved[1] != 1 && !(e.type == 0x357c && saved[1] == 11)))
                        throw std::runtime_error("Buffer view has an incompatible dimension");
                    const auto &d = source.desc;
                    const uint32_t bind = e.type == 0x357c ? 8 : e.type == 0x357d ? 128 : 32;
                    if (!(d.at(2) & bind))
                        throw std::runtime_error("Buffer lacks the view bind flag");
                    const auto flags =
                        e.type == 0x357d || (e.type == 0x357c && saved[1] == 11) ? saved[4] : 0;
                    if ((e.type == 0x357c && flags > 1) ||
                        (e.type == 0x357d && flags != 0 && flags != 1 && flags != 2 && flags != 4))
                        throw std::runtime_error("Invalid buffer view flags");
                    uint32_t stride = 0;
                    if (flags == 1) {
                        if (saved[0] != 39 || !(d.at(4) & 32) || (d.at(4) & 64))
                            throw std::runtime_error("Raw view requires a raw buffer and R32_TYPELESS");
                        stride = 4;
                    } else if (d.at(4) & 64) {
                        stride = d.at(5);
                        if (e.type == 0x357e || saved[0] != 0 || !stride || stride % 4 || stride > 2048)
                            throw std::runtime_error("Structured view format or stride is invalid");
                    } else {
                        if (!saved[0] || flags)
                            throw std::runtime_error("Typed buffer view format or counter flags are invalid");
                        const auto layout = pitches(1, 1, saved[0]);
                        stride = layout.first;
                        if (layout.second != 1 || !stride || stride > 16)
                            throw std::runtime_error("Unsupported typed buffer element layout");
                    }
                    if (!saved[3] || (uint64_t(saved[2]) + saved[3]) * stride > d.at(0))
                        throw std::runtime_error("Buffer view element range exceeds saved storage");
                }
                if (c.hasDescriptor && !createdViewDescriptorEqual(e.type, saved, c.descriptor))
                    throw std::runtime_error("Created view descriptor differs from its saved resource");
            } else {
                if (!c.hasDescriptor || resource.type != uint16_t(0x84 + e.type - 0x3579) ||
                    resource.device != c.device)
                    throw std::runtime_error(
                        "Texture creation descriptor, resource kind or device is missing/inconsistent");
                if (c.descriptor != resource.desc)
                    throw std::runtime_error("Texture creation descriptor differs from the saved resource; "
                                             "implicit descriptor normalization is not recovered");
                const auto info = textureInfo(resource);
                const auto usage = c.descriptor[c.descriptor.size() - 4];
                if (usage > 3 || (usage == 1 && !c.hasInitial))
                    throw std::runtime_error("Texture creation usage or immutable initialization is invalid");
                const auto subs = textureSubresources(resource);
                if (c.hasInitial) {
                    if (info.samples != 1 || !resource.data || info.format == 104 || info.format == 105)
                        throw std::runtime_error(
                            "Creation-time texture bytes are missing or use an unresolved captured encoding");
                    for (const auto &v : c.initial)
                        if (!v.pointer)
                            throw std::runtime_error(
                                "Texture initial observation has a null process pointer");
                    for (size_t i = 0; i < c.initial.size(); ++i)
                        if ((e.type != 0x3579 && !c.initial[i].rowPitch) ||
                            (e.type == 0x357b && !c.initial[i].slicePitch))
                            throw std::runtime_error(
                                "Texture initial data has a zero required row/slice pitch");
                    const auto data = frame.data(resource.data);
                    if (data.size() != subs.back().offset + subs.back().size)
                        throw std::runtime_error(
                            "Texture initial byte count does not cover all captured subresources");
                    c.data = resource.data;
                }
                // No initial flag: never substitute a later first-use blob for unspecified contents.
            }
        } catch (const std::exception &ex) {
            c.error = ex.what();
        }
    }
    return audit;
}
const TextureCreationRecord &requireTextureCreation(const TextureCreationAudit &audit, Id event) {
    const auto &c = audit.records.at(event);
    if (!c.error.empty())
        throw std::runtime_error("Texture/view creation event " + std::to_string(event) + ", resource " +
                                 std::to_string(c.resource) + ": " + c.error);
    return c;
}
bool isTextureCreationObservation(uint16_t t) {
    return t == 0x3015 || t == 0x301c || viewCreationObservation(t) != ViewObservation::None || t == 0x3134 ||
           t == 0x3135 || t == 0x3136 || t == 0x3137 || t == 0x313e || t == 0x3007 || t == 0x3008 ||
           t == 0x3009 || t == 0x300a || t == 0x3011 || t == 0x313f || t == 0x3140 || t == 0x3141 ||
           t == 0x3142 || t == 0x3149 || t == 0x3592 || t == 0x35aa;
}
bool acceptTextureCreationObservation(uint16_t t, Bytes bytes) {
    if (!isTextureCreationObservation(t))
        return false;
    Reader r(bytes);
    r.skip(16); // Link/owner are observations, not replay COM calls.
    const auto view = viewCreationObservation(t);
    if (view != ViewObservation::None) {
        switch (view) {
        case ViewObservation::QueryInterface:
            r.skip(28);
            break;
        case ViewObservation::ReferenceCount:
            r.skip(4);
            break;
        case ViewObservation::GetDevice:
        case ViewObservation::GetResource:
            r.skip(8);
            break;
        case ViewObservation::GetDescriptor:
            if (r.flag())
                r.skip(t == 0x3025 ? 24 : 20);
            break;
        default:
            break;
        }
        r.end();
        return true;
    }
    switch (t) {
    case 0x3134:
    case 0x3007:
    case 0x313f:
        r.skip(4 + 16 + 8);
        break;
    case 0x3140:
    case 0x3141:
    case 0x3135:
    case 0x3136:
    case 0x3008:
    case 0x3009:
        r.skip(4);
        break;
    case 0x3015:
    case 0x3142:
    case 0x3137:
    case 0x300a:
        r.skip(8);
        break;
    case 0x301c:
    case 0x3149:
    case 0x313e:
    case 0x3011:
        if (r.flag())
            r.skip(t == 0x301c ? 24 : t == 0x313e ? 32 : t == 0x3011 ? 36 : 44);
        break;
    case 0x3592:
        r.skip(8);
        if (r.flag())
            r.skip(4);
        break;
    case 0x35aa:
        r.skip(16);
        if (r.flag())
            r.skip(4);
        break;
    }
    r.end();
    return true;
}
} // namespace flora
