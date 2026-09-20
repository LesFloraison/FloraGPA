#include "Frame.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <limits>

namespace flora {
void Frame::close() noexcept {
    if (data_)
        UnmapViewOfFile(data_);
    if (mapping_)
        CloseHandle(mapping_);
    if (file_ && file_ != INVALID_HANDLE_VALUE)
        CloseHandle(file_);
    data_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
}
Frame::Frame(const std::filesystem::path &path) : path_(path) {
    try {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open capture (Windows error " + std::to_string(GetLastError()) +
                                     ")");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_, &length) || length.QuadPart < 0x128)
            throw std::runtime_error("Capture header is truncated");
        size_ = static_cast<uint64_t>(length.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_)
            throw std::runtime_error("Cannot map capture");
        data_ = static_cast<const uint8_t *>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_)
            throw std::runtime_error("Cannot read capture mapping");
        Reader header({data_, 0x128});
        if (header.read<uint32_t>() != 0x41504749 || header.read<uint32_t>() != 0x128 ||
            header.read<uint32_t>() != 3)
            throw std::runtime_error("Unsupported IGPA header");
        auto count = header.read<uint32_t>();
        if (std::memcmp(data_ + 0x44, "DX11", 4))
            throw std::runtime_error("Not a DX11 capture");
        Reader tail({data_ + 0xf4, 0x34});
        auto table = tail.read<uint64_t>();
        if (count > 1'000'000 || table < 0x128 || table > size_ || uint64_t(count) * 24 > size_ - table)
            throw std::runtime_error("Invalid entry table bounds");
        tail.skip(0x120 - 0xfc);
        width_ = tail.read<uint32_t>();
        height_ = tail.read<uint32_t>();
        Reader entries({data_ + table, size_t(count) * 24});
        for (uint32_t i = 0; i < count; ++i) {
            Entry e{entries.read<Id>(),      entries.read<uint64_t>(), entries.read<uint32_t>(),
                    entries.read<uint8_t>(), entries.read<uint8_t>(),  entries.read<uint16_t>()};
            if (e.offset < 0x128 || e.offset > table || e.size > table - e.offset ||
                !entries_.emplace(e.id, e).second)
                throw std::runtime_error("Invalid or duplicate entry " + std::to_string(e.id));
            entryOrder_.push_back(e.id);
        }
    } catch (...) {
        close();
        throw;
    }
}
const Entry &Frame::entry(Id id) const {
    auto it = entries_.find(id);
    if (it == entries_.end())
        throw std::runtime_error("Missing capture entry " + std::to_string(id));
    return it->second;
}
Bytes Frame::payload(Id id, int category, int type) const {
    const auto &e = entry(id);
    if ((category >= 0 && e.category != category) || (type >= 0 && e.type != type))
        throw std::runtime_error("Unexpected record type for " + std::to_string(id));
    return {data_ + e.offset, e.size};
}
Bytes Frame::data(Id id) const {
    Reader r(payload(id, 9, 1));
    auto size = r.read<uint32_t>();
    auto b = r.take(size);
    r.end();
    return b;
}
Bytes Frame::shader(Id id) const {
    Reader r(payload(id, 9, 0x81));
    auto n = r.read<uint64_t>();
    if (n < 4 || n > r.remaining())
        throw std::runtime_error("Invalid shader length");
    auto b = r.take(size_t(n));
    if (std::memcmp(b.data(), "DXBC", 4))
        throw std::runtime_error("Not DXBC");
    r.skip(8);
    r.end();
    return b;
}
Resource Frame::resource(Id id) const {
    auto &e = entry(id);
    Reader r(payload(id, 5));
    Resource out{id, r.read<Id>(), r.read<Id>(), 0, e.type, {}};
    int fields = e.type == 0x83                       ? 6
                 : e.type == 0x84                     ? 8
                 : e.type == 0x86                     ? 9
                 : (e.type == 0x85 || e.type == 0x87) ? 11
                                                      : 0;
    for (int i = 0; i < fields; ++i)
        out.desc.push_back(r.read<uint32_t>());
    if (fields) {
        out.data = r.read<Id>();
        r.end();
    } else if (e.type >= 0x90 && e.type <= 0x95) {
        r.skip(32);
        out.data = r.read<Id>();
        r.end();
    }
    return out;
}
State Frame::state(Id id) const {
    auto p = payload(id, 3, 3);
    if (p.size() != 22320)
        throw std::runtime_error("Unsupported state block size");
    Reader r(p);
    State s;
    s.mask = r.array<uint32_t, 32>();
    s.ib = r.read<Id>();
    s.ibFormat = r.read<uint32_t>();
    s.ibOffset = r.read<uint32_t>();
    s.layout = r.read<Id>();
    s.topology = r.read<uint32_t>();
    s.vb = r.array<Id, 32>();
    s.strides = r.array<uint32_t, 32>();
    s.offsets = r.array<uint32_t, 32>();
    auto stage = [&] {
        return Stage{r.array<Id, 14>(),  r.array<Id, 16>(),  r.read<Id>(),
                     r.array<Id, 128>(), r.array<Id, 256>(), r.read<uint32_t>()};
    };
    for (int i = 0; i < 4; ++i)
        s.stages[i] = stage();
    s.so = r.array<Id, 4>();
    s.soOffsets = r.array<uint32_t, 4>();
    s.soCounts = r.array<uint32_t, 4>();
    s.soCount = r.read<uint32_t>();
    s.scissors = r.read<Id>();
    s.rasterizer = r.read<Id>();
    s.viewports = r.read<Id>();
    s.stages[4] = stage();
    s.blend = r.read<Id>();
    s.blendFactor = r.array<float, 4>();
    s.sampleMask = r.read<uint32_t>();
    s.depthState = r.read<Id>();
    s.stencilRef = r.read<uint32_t>();
    s.rtv = r.array<Id, 8>();
    s.omStart = r.read<uint32_t>();
    s.dsv = r.read<Id>();
    s.omCounts = r.array<uint32_t, 8>();
    s.rtCount = r.read<uint32_t>();
    s.stages[5] = stage();
    s.csStart = r.read<uint32_t>();
    s.csCount = r.read<uint32_t>();
    s.csUav = r.array<Id, 8>();
    s.csCounts = r.array<uint32_t, 8>();
    s.predicate = r.read<Id>();
    s.predicateValue = r.read<uint32_t>();
    r.skip(4);
    s.omExtended = r.array<Id, 56>();
    s.omExtendedCounts = r.array<uint32_t, 56>();
    s.csExtended = r.array<Id, 56>();
    s.csExtendedCounts = r.array<uint32_t, 56>();
    r.end();
    return s;
}
bool isDraw(uint16_t t) { return t >= 0x35 && t <= 0x3d; }
Event Frame::event(Id id) const {
    auto &e = entry(id);
    if (!isDraw(e.type))
        throw std::runtime_error("Not a draw/dispatch event");
    Reader r(payload(id, 7));
    Event out;
    out.id = id;
    out.type = e.type;
    out.state = r.read<Id>();
    if (r.read<Id>())
        throw std::runtime_error("Unsupported draw flags");
    out.context = r.read<Id>();
    if (e.type == 0x36 || e.type == 0x3b || e.type == 0x3d)
        out.argumentBuffer = r.read<Id>();
    const int n = e.type == 0x38                                         ? 0
                  : (e.type == 0x36 || e.type == 0x3b || e.type == 0x3d) ? 1
                  : e.type == 0x37                                       ? 2
                  : e.type == 0x3a                                       ? 5
                  : e.type == 0x3c                                       ? 4
                                                                         : 3;
    for (int i = 0; i < n; ++i)
        out.args.push_back(r.read<uint32_t>());
    r.end();
    return out;
}
std::vector<std::pair<size_t, Bytes>> Frame::updates(Id id, size_t size) const {
    if (entry(id).type == 1) {
        auto b = data(id);
        if (b.size() != size)
            throw std::runtime_error("Full resource update length mismatch");
        return {{0, b}};
    }
    Reader r(payload(id, 9, 0x100));
    auto headers = r.read<uint32_t>(), bytes = r.read<uint32_t>();
    if (headers % 8 || uint64_t(headers) + bytes != r.remaining())
        throw std::runtime_error("Invalid resource diff header");
    Reader ranges(r.take(headers));
    std::vector<std::pair<size_t, Bytes>> out;
    while (ranges.remaining()) {
        auto start = ranges.read<uint32_t>(), n = ranges.read<uint32_t>();
        if (start > size || n > size - start)
            throw std::runtime_error("Resource diff exceeds storage");
        out.emplace_back(start, r.take(n));
    }
    r.end();
    return out;
}
std::string commandName(uint16_t type) {
    static const std::map<int, std::string> names{
#include "CommandNames.inc"
    };
    auto it = names.find(type);
    if (it != names.end())
        return it->second;
    return "Unknown (" + std::to_string(type) + ")";
}
std::string resourceName(uint16_t t) {
    static const std::map<int, std::string> names{{0x82, "Input layout"},
                                                  {0x83, "Buffer"},
                                                  {0x84, "Texture 1D"},
                                                  {0x85, "Texture 2D"},
                                                  {0x86, "Texture 3D"},
                                                  {0x87, "Reference image"},
                                                  {0x88, "Sampler"},
                                                  {0x89, "Rasterizer"},
                                                  {0x8a, "Blend"},
                                                  {0x8b, "Depth / stencil"},
                                                  {0x8c, "SRV"},
                                                  {0x8d, "RTV"},
                                                  {0x8e, "DSV"},
                                                  {0x8f, "UAV"},
                                                  {0x90, "Vertex shader"},
                                                  {0x91, "Geometry shader"},
                                                  {0x92, "Pixel shader"},
                                                  {0x93, "Compute shader"},
                                                  {0x94, "Domain shader"},
                                                  {0x95, "Hull shader"},
                                                  {0x96, "Predicate"},
                                                  {0x97, "Class linkage"},
                                                  {0x98, "Class instance"},
                                                  {0x10d, "Blend"},
                                                  {0x10e, "Rasterizer"},
                                                  {0x10f, "Rasterizer"}};
    auto it = names.find(t);
    return it == names.end() ? "Resource" : it->second;
}
TextureInfo textureInfo(const Resource &r) {
    const auto &d = r.desc;
    if (r.type == 0x84)
        return {d.at(0), 1, 1, d.at(1), d.at(2), d.at(3), 1, 2};
    if (r.type == 0x86)
        return {d.at(0), d.at(1), d.at(2), d.at(3), 1, d.at(4), 1, 4};
    if (r.type == 0x85 || r.type == 0x87)
        return {d.at(0), d.at(1), 1, d.at(2), d.at(3), d.at(4), d.at(5), 3};
    throw std::runtime_error("Not a texture");
}
std::pair<uint32_t, uint32_t> pitches(uint32_t w, uint32_t h, uint32_t f) {
    uint64_t row = 0;
    uint32_t rows = h;
    if (f >= 103 && f <= 105) {
        if (!w || !h || w % 2 || h % 2)
            throw std::runtime_error("Invalid planar dimensions");
        row = uint64_t(w) * (f == 103 ? 1 : 2);
        if (h > UINT32_MAX / 3 * 2)
            throw std::runtime_error("Planar size overflow");
        rows = h + h / 2;
    } else if ((f >= 70 && f <= 84) || (f >= 94 && f <= 99)) {
        row = std::max<uint64_t>(1, (uint64_t(w) + 3) / 4) * ((f <= 72 || (f >= 79 && f <= 81)) ? 8 : 16);
        rows = uint32_t(std::max<uint64_t>(1, (uint64_t(h) + 3) / 4));
    } else {
        auto bits = f >= 1 && f <= 4                    ? 128
                    : f <= 8 && f >= 5                  ? 96
                    : f >= 9 && f <= 22                 ? 64
                    : f >= 23 && f <= 47                ? 32
                    : f >= 48 && f <= 59                ? 16
                    : f >= 60 && f <= 65                ? 8
                    : f == 66                           ? 1
                    : (f == 67 || (f >= 87 && f <= 93)) ? 32
                    : (f == 85 || f == 86 || f == 115)  ? 16
                                                        : 0;
        if (!bits)
            throw std::runtime_error("Unsupported DXGI storage format " + std::to_string(f));
        row = (uint64_t(w) * bits + 7) / 8;
    }
    if (row > UINT32_MAX)
        throw std::runtime_error("Texture pitch overflow");
    return {uint32_t(row), rows};
}
} // namespace flora
