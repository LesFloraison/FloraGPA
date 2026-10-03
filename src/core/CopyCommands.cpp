#include "CopyCommands.h"
#include "Contexts.h"
#include "TextureCopies.h"
#include "UavCounters.h"
namespace flora {
bool isCopyCommand(uint16_t type) {
    return type == 0x3e || type == 0x3f || type == 0x40 || type == 0x42 || type == 0x256;
}
CopyCommand readCopyCommand(uint16_t type, Bytes payload) {
    if (!isCopyCommand(type))
        throw std::runtime_error("Not a copy command");
    Reader r(payload);
    if (r.read<Id>())
        throw std::runtime_error("Linked copy execution is unresolved");
    CopyCommand c;
    c.type = type;
    const bool region = type == 0x40 || type == 0x256;
    c.context = r.read<Id>();
    c.destination = r.read<Id>();
    if (type == 0x3f)
        c.x = r.read<uint32_t>();
    if (region || type == 0x42)
        c.destinationSubresource = r.read<uint32_t>();
    if (region) {
        c.x = r.read<uint32_t>();
        c.y = r.read<uint32_t>();
        c.z = r.read<uint32_t>();
    }
    c.source = r.read<Id>();
    if (region || type == 0x42)
        c.sourceSubresource = r.read<uint32_t>();
    if (type == 0x42)
        c.format = r.read<uint32_t>();
    if (region) {
        if (r.flag())
            c.box = r.array<uint32_t, 6>();
    }
    if (type == 0x256) {
        c.flags = r.read<uint32_t>();
        if (c.flags > 2)
            throw std::runtime_error("Invalid CopySubresourceRegion1 flags");
    }
    r.end();
    return c;
}
CopyValidation validateCopyCommand(const Frame &frame, const CopyCommand &c) {
    try {
        if (!isCopyCommand(c.type))
            throw std::runtime_error("Not a supported copy command");
        requireImmediateContext(frame, c.context);
        auto resource = [&](Id id) {
            auto r = frame.resource(id);
            if (r.type < 0x83 || r.type > 0x86)
                throw std::runtime_error("Copy operand is not a buffer or texture");
            if (r.type == 0x83 && !r.desc.at(0))
                throw std::runtime_error("Buffer ByteWidth is zero");
            return r;
        };
        const auto dst = resource(c.destination);
        const auto usage = dst.desc.at(dst.type == 0x83 ? 1 : dst.desc.size() - 4);
        if (usage == 1)
            throw std::runtime_error("Copy destination is immutable");
        if (c.type == 0x3f) {
            if (dst.type != 0x83)
                throw std::runtime_error("CopyStructureCount destination is not a buffer");
            if (c.x % 4 || uint64_t(c.x) + 4 > dst.desc[0])
                throw std::runtime_error("CopyStructureCount offset is unaligned or outside the destination");
            const auto counter = describeCounter(frame, c.source);
            if (!counter)
                throw std::runtime_error("CopyStructureCount source is not an APPEND/COUNTER structured UAV");
            const auto src = resource(counter->resource);
            if (!(src.desc[2] & 128) || counter->stride > 2048 || counter->stride % 4 ||
                src.desc[0] % counter->stride || !counter->numElements)
                throw std::runtime_error("Counter source buffer or stride is invalid");
            const auto elements = src.desc[0] / counter->stride;
            if (counter->firstElement > elements || counter->numElements > elements - counter->firstElement)
                throw std::runtime_error("Counter UAV element range exceeds its buffer");
            return CopyValidation::Buffer;
        }
        const auto src = resource(c.source);
        if (src.type != dst.type)
            throw std::runtime_error("Copy resource dimensions differ");
        if (c.type == 0x3e && c.destination == c.source)
            throw std::runtime_error("CopyResource requires different resources");
        if (dst.type != 0x83)
            return validateTextureCopy(src, dst, c);
        if (c.type == 0x42)
            throw std::runtime_error("ResolveSubresource requires 2D multisample textures");
        if (c.type == 0x3e) {
            if (dst.desc[0] != src.desc[0])
                throw std::runtime_error("CopyResource buffer ByteWidth mismatch");
            return CopyValidation::Buffer;
        }
        if (c.destinationSubresource || c.sourceSubresource)
            throw std::runtime_error("Buffer copy subresource must be zero");
        if (c.box &&
            ((*c.box)[0] >= (*c.box)[3] || (*c.box)[1] >= (*c.box)[4] || (*c.box)[2] >= (*c.box)[5])) {
            if (c.x > dst.desc[0] || c.y || c.z || (*c.box)[0] > src.desc[0] || (*c.box)[3] > src.desc[0] ||
                (*c.box)[1] > 1 || (*c.box)[2] > 1 || (*c.box)[4] > 1 || (*c.box)[5] > 1)
                throw std::runtime_error("Empty buffer copy still has invalid resource coordinates");
            return CopyValidation::Buffer; // D3D11 defines bounded empty boxes as no-ops, including inverted
                                           // axes.
        }
        if (c.source == c.destination && c.type != 0x256)
            throw std::runtime_error("Same-buffer region copy is outside the verified scope");
        const auto box = c.box.value_or(std::array<uint32_t, 6>{0, 0, 0, src.desc[0], 1, 1});
        if (box[1] || box[2] || box[4] != 1 || box[5] != 1 || c.y || c.z)
            throw std::runtime_error("Buffer region copy requires one-dimensional byte coordinates");
        if (box[3] > src.desc[0] || uint64_t(c.x) + (box[3] - box[0]) > dst.desc[0])
            throw std::runtime_error("Buffer region copy exceeds source or destination ByteWidth");
        return CopyValidation::Buffer;
    } catch (const std::exception &e) {
        throw std::runtime_error("Copy destination " + std::to_string(c.destination) + ", source " +
                                 std::to_string(c.source) + ": " + e.what());
    }
}
} // namespace flora
