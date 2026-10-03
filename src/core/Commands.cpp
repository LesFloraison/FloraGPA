#include "Commands.h"
#include "ClearView.h"
#include <algorithm>
#include <limits>

namespace flora {
void validateAnnotationCommand(uint16_t type, Bytes payload) {
    if (type < 0x3278 || type > 0x327e)
        throw std::runtime_error("Not an annotation command");
    Reader r(payload);
    r.skip(16);
    if (type == 0x3278)
        r.skip(28);
    else if (type == 0x3279 || type == 0x327a || type == 0x327e)
        r.skip(4);
    if (type == 0x327b || type == 0x327c)
        r.skip(4);
    if (type == 0x327b || type == 0x327d) {
        const auto size = r.read<uint32_t>();
        if (size % 2 || size > 0x8002)
            throw std::runtime_error("Annotation name length must be even and at most 32770 bytes");
        r.skip(size);
    }
    r.end();
}
bool isClearCommand(uint16_t type) { return type >= 0x31 && type <= 0x34; }
bool isWritableCommand(uint16_t type) {
    return isClearCommand(type) || type == 0x3e || type == 0x3f || type == 0x40 || type == 0x42 ||
           type == 0x245 || type == 0x246 || type == 0x247 || type == 0x257;
}
void validateWritableCommand(const Frame &frame, Id event) {
    auto &e = frame.entry(event);
    if (e.category != 7 || !isWritableCommand(e.type))
        throw std::runtime_error("Select a recovered resource-writing command");
    if (e.type == 0x257) {
        validateClearView(frame, readClearView(frame.payload(event)));
        return;
    }
    Reader r(frame.payload(event));
    r.skip(8);
    requireImmediateContext(frame, r.read<Id>());
    switch (e.type) {
    case 0x31:
        r.skip(17);
        break;
    case 0x32:
    case 0x33:
    case 0x34:
        r.skip(8);
        if (r.flag())
            r.skip(16);
        break;
    case 0x3e:
        r.skip(16);
        break;
    case 0x3f:
        r.skip(20);
        break;
    case 0x40:
        r.skip(36);
        if (r.flag())
            r.skip(24);
        break;
    case 0x42:
        r.skip(28);
        break;
    case 0x245:
        r.skip(8);
        break;
    case 0x246:
        r.skip(32);
        break;
    case 0x247:
        r.skip(12);
        if (r.flag())
            r.skip(24);
        r.skip(16);
        break;
    }
    r.end();
}
UpdateSourceLayout updateSourceLayout(const Frame &frame, Id event) {
    if (frame.entry(event).type != 0x247)
        throw std::runtime_error("Source replacement requires UpdateSubresource");
    validateWritableCommand(frame, event);
    Reader r(frame.payload(event));
    r.skip(16);
    UpdateSourceLayout out;
    out.destination = r.read<Id>();
    out.subresource = r.read<uint32_t>();
    out.hasBox = r.flag();
    if (out.hasBox)
        out.box = r.array<uint32_t, 6>();
    out.data = r.read<Id>();
    r.skip(8); // Captured pitches are not pitches of the tightly packed asset.
    r.end();
    auto resource = frame.resource(out.destination);
    uint32_t format = 0;
    if (resource.type == 0x83) {
        if (out.subresource)
            throw std::runtime_error("Buffer Update subresource must be zero");
        out.width = resource.desc.at(0);
        out.height = out.depth = 1;
    } else {
        auto info = textureInfo(resource);
        if (!info.mips || info.mips > 32 || info.samples != 1 ||
            out.subresource >= uint64_t(info.mips) * info.layers)
            throw std::runtime_error("Update requires an explicit non-MSAA subresource");
        auto mip = out.subresource % info.mips;
        out.width = std::max(1u, info.width >> mip);
        out.height = std::max(1u, info.height >> mip);
        out.depth = std::max(1u, info.depth >> mip);
        format = info.format;
        if (format >= 103 && format <= 105) {
            if (info.dimension != 3 || info.mips != 1 || (resource.desc.at(10) & 4))
                throw std::runtime_error("Planar Update requires non-cube 2D storage with one mip");
            pitches(info.width, info.height, format);
        }
    }
    if (out.hasBox) {
        auto [left, top, front, right, bottom, back] = out.box;
        if (!(left < right && right <= out.width && top < bottom && bottom <= out.height && front < back &&
              back <= out.depth))
            throw std::runtime_error("Update source box must be nonempty and in bounds");
        bool bc = (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
        if (format >= 103 && format <= 105 && (left % 2 || top % 2 || right % 2 || bottom % 2))
            throw std::runtime_error("Planar Update box requires even coordinates");
        if (bc && (left % 4 || top % 4 || (right % 4 && right != out.width) ||
                   (bottom % 4 && bottom != out.height)))
            throw std::runtime_error("BC Update box must align to blocks or texture edges");
        out.width = right - left;
        out.height = bottom - top;
        out.depth = back - front;
    }
    auto [row, rows] =
        resource.type == 0x83 ? std::pair(out.width, 1u) : pitches(out.width, out.height, format);
    uint64_t slice = uint64_t(row) * rows;
    if (!row || !slice || slice > UINT32_MAX || slice * out.depth > SIZE_MAX)
        throw std::runtime_error("Update source size overflow");
    out.rowPitch = row;
    out.slicePitch = uint32_t(slice);
    out.size = slice * out.depth;
    return out;
}
} // namespace flora
