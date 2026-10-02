#include "TextureCreation.h"
#include "TextureEdits.h"
namespace flora {
bool isTextureCreation(uint16_t t) { return t >= 0x3579 && t <= 0x357c; }
bool textureSrvDescriptorEqual(std::span<const uint32_t> a, std::span<const uint32_t> b) {
    if (a.size() != 6 || b.size() != 6 || a[0] != b[0] || a[1] != b[1])
        return false;
    // Compare the active union only; padding and inactive descriptor members are not semantics.
    const auto words = a[1] == 4 || a[1] == 7 || a[1] == 9 ? 4
                       : a[1] == 5 || a[1] == 10           ? 6
                       : a[1] == 6                         ? 2
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
    if (t == 0x357c)
        out.source = r.read<Id>();
    out.hasDescriptor = r.flag();
    const auto fields = t == 0x3579 ? 8 : t == 0x357a ? 11 : t == 0x357b ? 9 : 6;
    if (out.hasDescriptor)
        for (int i = 0; i < fields; ++i)
            out.descriptor.push_back(r.read<uint32_t>());
    if (t != 0x357c) {
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
            if (e.type == 0x357c) {
                Reader r(frame.capturedPayload(c.resource, 5, 0x8c));
                r.skip(16);
                if (r.read<Id>() != c.source)
                    throw std::runtime_error("Created SRV source differs from its saved resource");
                const auto saved = r.array<uint32_t, 6>();
                r.end();
                const auto source = frame.resource(c.source);
                if (resource.device != c.device || source.device != c.device || source.type != 0x85)
                    throw std::runtime_error(
                        "Captured SRV creation requires a Texture2D source on the recorded device");
                if (c.hasDescriptor && !textureSrvDescriptorEqual(saved, c.descriptor))
                    throw std::runtime_error("Created SRV descriptor differs from its saved resource");
            } else {
                if (e.type != 0x357a)
                    throw std::runtime_error("Texture1D/Texture3D creation wire is decoded; execution awaits "
                                             "original capture acceptance");
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
                        if (c.initial[i].rowPitch < subs[i].rowPitch)
                            throw std::runtime_error(
                                "Texture initial row pitch is shorter than a captured row");
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
    return t == 0x313f || t == 0x3140 || t == 0x3141 || t == 0x3142 || t == 0x3149 || t == 0x3592 ||
           t == 0x35aa;
}
bool acceptTextureCreationObservation(uint16_t t, Bytes bytes) {
    if (!isTextureCreationObservation(t))
        return false;
    Reader r(bytes);
    r.skip(16); // Link/owner are observations, not replay COM calls.
    switch (t) {
    case 0x313f:
        r.skip(4 + 16 + 8);
        break;
    case 0x3140:
    case 0x3141:
        r.skip(4);
        break;
    case 0x3142:
        r.skip(8);
        break;
    case 0x3149:
        if (r.flag())
            r.skip(44);
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
