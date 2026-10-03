#include "ClearView.h"
#include "Contexts.h"
namespace flora {
ClearViewCommand readClearView(Bytes bytes) {
    Reader r(bytes);
    if (r.read<Id>())
        throw std::runtime_error("Linked ClearView execution is unresolved");
    ClearViewCommand c;
    c.context = r.read<Id>();
    c.view = r.read<Id>();
    if (!r.flag())
        throw std::runtime_error("ClearView color array is missing");
    c.color = r.array<float, 4>();
    c.count = r.read<uint32_t>();
    c.hasRectangles = r.flag();
    if (c.hasRectangles) {
        if (!c.count || c.count > 65536)
            throw std::runtime_error("ClearView rectangle count outside supported bounds");
        Reader rectangles(r.take(size_t(c.count) * 16));
        c.rectangles.reserve(c.count);
        for (uint32_t i = 0; i < c.count; i++)
            c.rectangles.push_back(rectangles.array<int32_t, 4>());
    }
    r.end();
    return c;
}
ClearViewTarget validateClearView(const Frame &frame, const ClearViewCommand &c) {
    try {
        requireImmediateContext(frame, c.context);
        const auto &e = frame.entry(c.view);
        if (e.category != 5 || (e.type != 0x8d && e.type != 0x8e && e.type != 0x8f))
            throw std::runtime_error(
                "ClearView requires an RTV, depth-only DSV or UAV; video views are unsupported");
        Reader v(frame.payload(c.view));
        v.skip(16);
        ClearViewTarget target{v.read<Id>(), e.type, v.read<uint32_t>(), v.read<uint32_t>()};
        const auto flags = e.type == 0x8e ? v.read<uint32_t>() : 0;
        const auto fields = v.array<uint32_t, 3>();
        v.end();
        const auto resource = frame.resource(target.resource);
        if (resource.type < 0x83 || resource.type > 0x85)
            throw std::runtime_error("ClearView supports buffers and 1D/2D textures, not 3D resources");
        if (e.type == 0x8e && (flags || (target.format != 40 && target.format != 55)))
            throw std::runtime_error("ClearView requires a writable depth-only D32_FLOAT or D16_UNORM view");
        const auto dim = target.dimension;
        bool valid = false;
        if (resource.type == 0x83) {
            valid = e.type != 0x8e && dim == 1;
            if (!target.format || (e.type == 0x8f && fields[2]))
                throw std::runtime_error(
                    "Raw/structured/flagged buffer ClearView is outside the verified scope");
            for (const auto &rect : c.rectangles)
                if (rect[0] != rect[2] && rect[1] != rect[3] && (rect[1] != 0 || rect[3] != 1))
                    throw std::runtime_error("Buffer ClearView rectangles require top=0 and bottom=1");
        } else if (resource.type == 0x84)
            valid = e.type == 0x8e ? (dim == 1 || dim == 2) : (dim == 2 || dim == 3);
        else
            valid = e.type == 0x8e   ? dim >= 3 && dim <= 6
                    : e.type == 0x8f ? (dim == 4 || dim == 5)
                                     : dim >= 4 && dim <= 7;
        if (!valid)
            throw std::runtime_error("ClearView view/resource dimensions differ");
        for (const auto &rect : c.rectangles)
            if (rect[0] > rect[2] || rect[1] > rect[3])
                throw std::runtime_error("ClearView rectangle has inverted bounds");
        return target;
    } catch (const std::exception &e) {
        throw std::runtime_error("ClearView view " + std::to_string(c.view) + ": " + e.what());
    }
}
} // namespace flora
