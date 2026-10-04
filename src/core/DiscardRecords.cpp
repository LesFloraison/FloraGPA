#include "DiscardRecords.h"
#include "Contexts.h"
namespace flora {
bool isDiscardRecord(uint16_t type) { return type == 0x3553 || type == 0x3554 || type == 0x3563; }
DiscardRecord readDiscardRecord(uint16_t type, Bytes payload) {
    if (!isDiscardRecord(type))
        throw std::runtime_error("Not a recovered discard record");
    Reader r(payload);
    DiscardRecord c;
    c.type = type;
    c.link = r.read<Id>();
    c.context = r.read<Id>();
    c.target = r.read<Id>();
    if (type == 0x3563) {
        c.count = r.read<uint32_t>();
        c.hasRectangles = r.flag();
        if (c.hasRectangles) {
            if (!c.count || c.count > 65536)
                throw std::runtime_error("DiscardView1 rectangle count is outside inspected bounds");
            Reader rectangles(r.take(size_t(c.count) * 16));
            for (uint32_t i = 0; i < c.count; ++i)
                c.rectangles.push_back(rectangles.array<int32_t, 4>());
        }
    }
    r.end();
    return c;
}
bool ambiguousDiscardRectangles(const DiscardRecord &r) {
    return r.type == 0x3563 && !r.hasRectangles && !r.count;
}
DiscardTarget validateDiscardRecord(const Frame &frame, const DiscardRecord &c) {
    if (c.link)
        throw std::runtime_error("Linked discard execution is unresolved");
    requireImmediateContext(frame, c.context);
    if (!c.target)
        throw std::runtime_error(
            "Discard target identity is missing; a null native target cannot be inferred");
    DiscardTarget out{c.target, 0};
    std::vector<uint32_t> view;
    if (c.type != 0x3553) {
        const auto &entry = frame.entry(c.target);
        if (entry.category != 5 || entry.type < 0x8c || entry.type > 0x8f)
            throw std::runtime_error("Discard requires a saved SRV, RTV, DSV or UAV");
        out.viewType = entry.type;
        Reader r(frame.payload(c.target));
        r.skip(16);
        out.resource = r.read<Id>();
        const auto words = entry.type == 0x8c || entry.type == 0x8e ? 6 : 5;
        for (int i = 0; i < words; ++i)
            view.push_back(r.read<uint32_t>());
        r.end();
    }
    const auto resource = frame.resource(out.resource);
    if (resource.type < 0x83 || resource.type > 0x86)
        throw std::runtime_error("Discard target is not a saved buffer or 1D/2D/3D texture");
    const auto usage = resource.desc.at(resource.type == 0x83 ? 1 : resource.desc.size() - 4);
    if (usage != 0 && usage != 2)
        throw std::runtime_error(
            "Discard target requires DEFAULT or DYNAMIC usage; dropped invalid API calls are not replayed");
    if (out.viewType) {
        const auto dim = view[1];
        bool valid = false;
        if (resource.type == 0x83)
            valid = out.viewType != 0x8e && (dim == 1 || (out.viewType == 0x8c && dim == 11));
        else if (resource.type == 0x84)
            valid = out.viewType == 0x8e ? dim == 1 || dim == 2 : dim == 2 || dim == 3;
        else if (resource.type == 0x85)
            valid = out.viewType == 0x8c   ? (dim >= 4 && dim <= 7) || dim == 9 || dim == 10
                    : out.viewType == 0x8e ? dim >= 3 && dim <= 6
                    : out.viewType == 0x8f ? dim == 4 || dim == 5
                                           : dim >= 4 && dim <= 7;
        else
            valid = out.viewType != 0x8e && dim == 8;
        if (!valid)
            throw std::runtime_error("Discard view dimension conflicts with its resource");
        // Rectangle evidence currently covers typed buffers and ordinary 2D RTVs.
        // Other native view layouts retain their own driver validation for whole-view discard.
        if (c.hasRectangles &&
            !(resource.type == 0x83 && out.viewType == 0x8f && dim == 1 && view[0] == 42 && !view[4]) &&
            !(resource.type == 0x85 && out.viewType == 0x8d && dim == 4))
            throw std::runtime_error(
                "DiscardView1 rectangle/view combination is outside original acceptance evidence");
    }
    for (const auto &rect : c.rectangles) {
        if (rect[0] > rect[2] || rect[1] > rect[3])
            throw std::runtime_error("DiscardView1 rectangle has inverted bounds");
        if (resource.type == 0x83 && (rect[1] != 0 || rect[3] != 1))
            throw std::runtime_error("Buffer discard rectangles require top=0 and bottom=1");
    }
    if (ambiguousDiscardRectangles(c))
        throw std::runtime_error(
            "DiscardView1 zero-count record lost the rectangle pointer presence: whole-view discard and an "
            "empty non-null rectangle array cannot be distinguished; safe overwrite/use has not been proven");
    return out;
}
} // namespace flora
