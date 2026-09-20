#pragma once
// Shared bounded DXBC debug-container readers; captured paths are labels only.
#include "core/Frame.h"
#include <QByteArray>
#include <QStringDecoder>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>

namespace flora::shader_debug {
using Json = nlohmann::json;
constexpr size_t limit = 128 * 1024 * 1024;
inline Bytes sub(Bytes b, size_t offset, size_t size) {
    if (offset > b.size() || size > b.size() - offset)
        throw std::runtime_error("Shader data range exceeds storage");
    return b.subspan(offset, size);
}
inline std::pair<std::string, size_t> stringAt(Bytes bytes, size_t pos) {
    if (pos >= bytes.size())
        throw std::runtime_error("Shader string offset exceeds storage");
    auto end = std::find(bytes.begin() + pos, bytes.end(), uint8_t(0));
    if (end == bytes.end())
        throw std::runtime_error("Unterminated shader string");
    return {{reinterpret_cast<const char *>(bytes.data() + pos), size_t(end - (bytes.begin() + pos))},
            size_t(end - bytes.begin()) + 1};
}
inline std::map<std::string, Bytes> chunks(Bytes bytes) {
    Reader r(bytes);
    if (r.read<uint32_t>() != 0x43425844)
        throw std::runtime_error("Not DXBC");
    r.skip(16);
    if (r.read<uint32_t>() != 1 || r.read<uint32_t>() != bytes.size())
        throw std::runtime_error("DXBC header bounds");
    auto count = r.read<uint32_t>();
    if (count > 256)
        throw std::runtime_error("DXBC chunk limit");
    std::map<std::string, Bytes> result;
    for (uint32_t i = 0; i < count; ++i) {
        auto offset = r.read<uint32_t>();
        if (offset < 32 + count * 4)
            throw std::runtime_error("DXBC chunk overlaps header");
        Reader chunk(sub(bytes, offset, bytes.size() - std::min<size_t>(offset, bytes.size())));
        auto tag = chunk.take(4);
        std::string name(reinterpret_cast<const char *>(tag.data()), 4);
        auto data = chunk.take(chunk.read<uint32_t>());
        if (!result.emplace(name, data).second)
            throw std::runtime_error("Duplicate DXBC chunk");
    }
    return result;
}
struct DecodedText {
    std::string text, encoding = "utf-8";
    bool valid = true;
};
// Match Python's whole-buffer replacement decoding: one replacement per malformed
// subsequence, and never accept an incomplete final code unit as valid source.
inline DecodedText decodeSourceText(Bytes bytes, bool detectBom = true) {
    DecodedText decoded;
    std::u32string text;
    auto invalid = [&] {
        decoded.valid = false;
        text.push_back(0xfffd);
    };
    size_t pos = 0;
    if (bytes.size() >= 2 && detectBom &&
        ((bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == 0xfe && bytes[1] == 0xff))) {
        decoded.encoding = "utf-16";
        const bool little = bytes[0] == 0xff;
        auto unit = [&](size_t at) -> uint32_t {
            return little ? bytes[at] | (uint32_t(bytes[at + 1]) << 8)
                          : (uint32_t(bytes[at]) << 8) | bytes[at + 1];
        };
        pos = 2;
        while (pos < bytes.size()) {
            if (bytes.size() - pos < 2) {
                invalid();
                break;
            }
            auto value = unit(pos);
            pos += 2;
            if (value >= 0xd800 && value <= 0xdbff) {
                if (bytes.size() - pos < 2) {
                    invalid();
                    break;
                }
                auto low = unit(pos);
                if (low < 0xdc00 || low > 0xdfff) {
                    invalid();
                    continue;
                }
                pos += 2;
                value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
            } else if (value >= 0xdc00 && value <= 0xdfff) {
                invalid();
                continue;
            }
            text.push_back(value);
        }
    } else {
        if (detectBom && bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
            decoded.encoding = "utf-8-sig";
            pos = 3;
        }
        while (pos < bytes.size()) {
            const auto lead = bytes[pos++];
            if (lead < 0x80) {
                text.push_back(lead);
                continue;
            }
            const auto count = lead >= 0xc2 && lead <= 0xdf   ? 1u
                               : lead >= 0xe0 && lead <= 0xef ? 2u
                               : lead >= 0xf0 && lead <= 0xf4 ? 3u
                                                              : 0u;
            if (!count) {
                invalid();
                continue;
            }
            uint32_t value = lead & ((1u << (6 - count)) - 1);
            bool complete = true;
            for (unsigned i = 0; i < count; ++i) {
                if (pos == bytes.size()) {
                    complete = false;
                    break;
                }
                const auto next = bytes[pos];
                if (next < 0x80 || next > 0xbf ||
                    (i == 0 && ((lead == 0xe0 && next < 0xa0) || (lead == 0xed && next > 0x9f) ||
                                (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next > 0x8f)))) {
                    complete = false;
                    break;
                }
                ++pos;
                value = (value << 6) | (next & 63);
            }
            if (complete)
                text.push_back(value);
            else
                invalid();
        }
    }
    decoded.text = QString::fromUcs4(text.data(), qsizetype(text.size())).toStdString();
    return decoded;
}
inline Json sourceFile(const std::string &name, Bytes bytes) {
    const auto decoded = decodeSourceText(bytes);
    return {
        {"name",
         decodeSourceText(Bytes(reinterpret_cast<const uint8_t *>(name.data()), name.size()), false).text},
        {"byte_length", bytes.size()},
        {"sha256", sha256(bytes)},
        {"encoding", decoded.encoding},
        {"text_valid", decoded.valid},
        {"text", decoded.text},
        {"raw_hex", QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                        .toHex()
                        .toStdString()}};
}
inline std::string strictDebugText(const std::string &raw) {
    const auto decoded =
        decodeSourceText(Bytes(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()), false);
    if (!decoded.valid)
        throw std::runtime_error("Invalid UTF-8 debug metadata");
    return decoded.text;
}
class Pdb {
    Bytes data_;
    uint32_t page_{}, count_{};
    struct Stream {
        std::vector<uint32_t> pages;
        uint32_t size;
    };
    std::vector<Stream> streams_;
    Bytes block(uint32_t id) const {
        if (id >= count_)
            throw std::runtime_error("SPDB page outside file");
        return sub(data_, size_t(id) * page_, page_);
    }
    std::vector<uint8_t> collect(const std::vector<uint32_t> &ids, uint32_t size) const {
        std::vector<uint8_t> bytes;
        bytes.reserve(size);
        for (auto id : ids) {
            auto b = block(id);
            auto n = std::min<size_t>(b.size(), size - bytes.size());
            bytes.insert(bytes.end(), b.begin(), b.begin() + n);
        }
        if (bytes.size() != size)
            throw std::runtime_error("SPDB page data incomplete");
        return bytes;
    }

  public:
    explicit Pdb(Bytes bytes) : data_(bytes) {
        constexpr char magic[] = "Microsoft C/C++ MSF 7.00\r\n\x1a"
                                 "DS\0\0\0";
        if (bytes.size() < 56 || bytes.size() > limit || std::memcmp(bytes.data(), magic, 32))
            throw std::runtime_error("Unsupported SPDB MSF container");
        Reader r(bytes);
        r.skip(32);
        page_ = r.read<uint32_t>();
        auto free = r.read<uint32_t>();
        count_ = r.read<uint32_t>();
        auto size = r.read<uint32_t>(), reserved = r.read<uint32_t>(), mapping = r.read<uint32_t>();
        if ((page_ != 512 && page_ != 1024 && page_ != 2048 && page_ != 4096) ||
            uint64_t(count_) * page_ != bytes.size() || (free != 1 && free != 2) || reserved)
            throw std::runtime_error("SPDB superblock invalid");
        auto blocks = (uint64_t(size) + page_ - 1) / page_;
        if (size > bytes.size() || blocks * 4 > page_)
            throw std::runtime_error("SPDB directory exceeds limit");
        Reader map(block(mapping));
        std::vector<uint32_t> indices;
        for (size_t i = 0; i < blocks; ++i)
            indices.push_back(map.read<uint32_t>());
        auto raw = collect(indices, size);
        Reader directory(raw);
        auto count = directory.read<uint32_t>();
        if (count > 65536)
            throw std::runtime_error("SPDB stream count limit");
        std::vector<uint32_t> sizes;
        for (uint32_t i = 0; i < count; ++i)
            sizes.push_back(directory.read<uint32_t>());
        for (auto n : sizes) {
            Stream stream{{}, n};
            if (n != UINT32_MAX) {
                if (n > bytes.size())
                    throw std::runtime_error("SPDB stream exceeds file");
                auto pages = (uint64_t(n) + page_ - 1) / page_;
                for (size_t i = 0; i < pages; ++i) {
                    auto id = directory.read<uint32_t>();
                    block(id);
                    stream.pages.push_back(id);
                }
            }
            streams_.push_back(std::move(stream));
        }
        directory.end();
    }
    std::vector<uint8_t> stream(uint32_t id) const {
        if (id >= streams_.size() || streams_[id].size == UINT32_MAX)
            throw std::runtime_error("Missing SPDB stream");
        return collect(streams_[id].pages, streams_[id].size);
    }
    std::map<std::string, uint32_t> names() const {
        auto bytes = stream(1);
        Reader r(bytes);
        if (r.read<uint32_t>() != 20000404)
            throw std::runtime_error("Unsupported SPDB named stream version");
        r.skip(24);
        auto strings = r.take(r.read<uint32_t>());
        auto count = r.read<uint32_t>(), capacity = r.read<uint32_t>();
        if (count > capacity || capacity > 1048576)
            throw std::runtime_error("Invalid SPDB hash capacity");
        auto bits = [&] {
            auto words = r.read<uint32_t>();
            if (words > (capacity + 31) / 32)
                throw std::runtime_error("SPDB occupancy limit");
            std::set<uint32_t> result;
            for (uint32_t i = 0; i < words; ++i) {
                auto word = r.read<uint32_t>();
                for (uint32_t b = 0; b < 32; ++b)
                    if (word & (1u << b)) {
                        auto index = i * 32 + b;
                        if (index >= capacity)
                            throw std::runtime_error("SPDB occupancy out of bounds");
                        result.insert(index);
                    }
            }
            return result;
        };
        auto present = bits(), deleted = bits();
        if (present.size() != count)
            throw std::runtime_error("SPDB occupancy mismatch");
        for (auto index : present)
            if (deleted.contains(index))
                throw std::runtime_error("SPDB overlapping occupancy");
        std::map<std::string, uint32_t> result;
        for (auto index : present) {
            (void)index;
            auto offset = r.read<uint32_t>(), id = r.read<uint32_t>();
            auto name = stringAt(strings, offset).first;
            if (id >= streams_.size() || streams_[id].size == UINT32_MAX || !result.emplace(name, id).second)
                throw std::runtime_error("Invalid SPDB named stream");
        }
        return result;
    }
    Json environment() const {
        auto dbi = stream(3);
        Reader head(dbi);
        if (head.read<uint32_t>() != UINT32_MAX || head.read<uint32_t>() != 19990903 || dbi.size() < 64)
            throw std::runtime_error("Unsupported SPDB DBI header");
        head.skip(16);
        auto size = head.read<uint32_t>();
        auto modules = sub(dbi, 64, size);
        size_t pos = 0;
        Json result = Json::object();
        while (pos < modules.size()) {
            Reader module(sub(modules, pos, 64));
            module.skip(34);
            auto id = module.read<uint16_t>();
            auto symbolSize = module.read<uint32_t>();
            auto end = stringAt(modules, pos + 64).second;
            end = stringAt(modules, end).second;
            pos = (end + 3) & ~size_t(3);
            if (pos > modules.size())
                throw std::runtime_error("SPDB module alignment bounds");
            if (id == 0xffff)
                continue;
            auto data = stream(id);
            Reader symbols(sub(data, 0, symbolSize));
            if (symbols.read<uint32_t>() != 4)
                throw std::runtime_error("Unsupported CodeView signature");
            while (symbols.remaining()) {
                auto recordSize = symbols.read<uint16_t>();
                Reader record(symbols.take(recordSize));
                if (record.read<uint16_t>() != 0x113d)
                    continue;
                if (record.read<uint8_t>() != 1)
                    throw std::runtime_error("Unsupported HLSL environment");
                auto strings = record.take(record.remaining());
                size_t p = 0;
                while (p < strings.size() && strings[p]) {
                    auto [key, next] = stringAt(strings, p);
                    auto [value, last] = stringAt(strings, next);
                    key = strictDebugText(key);
                    value = strictDebugText(value);
                    p = last;
                    if (result.contains(key) && result[key] != value)
                        throw std::runtime_error("Conflicting compile metadata");
                    result[key] = value;
                }
                if (p == strings.size() ||
                    std::any_of(strings.begin() + p, strings.end(), [](uint8_t b) { return b != 0; }))
                    throw std::runtime_error("Invalid environment terminator");
            }
        }
        return result;
    }
};
inline Json embedded(const std::map<std::string, Bytes> &parts, bool lineFormat = false) {
    Json result{{"status", "original_source_not_embedded"},
                {"files", Json::array()},
                {"environment", Json::object()},
                {"issues", Json::array()}};
    try {
        if (parts.contains("SPDB")) {
            Pdb pdb(parts.at("SPDB"));
            size_t total = 0;
            for (auto &[name, id] : pdb.names())
                if (name.starts_with("/src/files/")) {
                    auto data = pdb.stream(id);
                    total += data.size();
                    if (total > limit)
                        throw std::runtime_error("Embedded source size limit");
                    auto item = sourceFile(name.substr(11), data);
                    item["stream"] = id;
                    if (lineFormat)
                        item["name_hex"] = QByteArray::fromStdString(name.substr(11)).toHex().toStdString();
                    result["files"].push_back(item);
                }
            try {
                result["environment"] = pdb.environment();
            } catch (const std::exception &e) {
                result["issues"].push_back(e.what());
            }
            result["status"] =
                result["files"].empty() ? "debug_present_without_source_files" : "embedded_source_available";
        } else if (parts.contains("SDBG")) {
            auto data = parts.at("SDBG");
            if (data.size() < 84 || data.size() > limit)
                throw std::runtime_error("Invalid SDBG size");
            Reader r(data);
            auto h = r.array<uint32_t, 21>();
            if (h[0] != 84)
                throw std::runtime_error("Unsupported SDBG header");
            auto base = data.subspan(84);
            auto ints = h[19], strings = h[20];
            if (ints > strings || strings > base.size() || ints % 4 || (strings - ints) % 4)
                throw std::runtime_error("SDBG datastore bounds");
            const uint32_t strides[] = {16, 396, 24, 24, 20, 20, 44};
            std::vector<std::pair<uint32_t, uint32_t>> intervals;
            for (int i = 0; i < 7; ++i) {
                auto count = h[5 + i * 2], offset = h[6 + i * 2];
                if (offset % 4 || offset > ints || count > (ints - offset) / strides[i])
                    throw std::runtime_error("SDBG table bounds");
                if (count)
                    intervals.emplace_back(offset, offset + count * strides[i]);
            }
            std::sort(intervals.begin(), intervals.end());
            for (size_t i = 1; i < intervals.size(); ++i)
                if (intervals[i - 1].second > intervals[i].first)
                    throw std::runtime_error("SDBG table overlap");
            auto ascii = base.subspan(strings);
            Reader files(sub(base, h[6], size_t(h[5]) * 16));
            size_t total = 0;
            for (uint32_t i = 0; i < h[5]; ++i) {
                auto name = files.read<uint32_t>(), length = files.read<uint32_t>(),
                     source = files.read<uint32_t>(), size = files.read<uint32_t>();
                auto n = sub(ascii, name, length);
                auto bytes = sub(ascii, source, size);
                total += size;
                if (total > limit)
                    throw std::runtime_error("SDBG source size limit");
                auto item = sourceFile({reinterpret_cast<const char *>(n.data()), n.size()}, bytes);
                item["sdbg_file"] = i;
                if (lineFormat) {
                    item["stream"] = nullptr;
                    item["name_hex"] =
                        QByteArray(reinterpret_cast<const char *>(n.data()), qsizetype(n.size()))
                            .toHex()
                            .toStdString();
                }
                result["files"].push_back(item);
            }
            result["environment"] = {{"hlslEntry", strictDebugText(stringAt(ascii, h[2]).first)},
                                     {"hlslTarget", strictDebugText(stringAt(ascii, h[3]).first)},
                                     {"hlslFlags", h[4]},
                                     {"hlslCompiler", strictDebugText(stringAt(ascii, h[1]).first)}};
            result["status"] =
                result["files"].empty() ? "debug_present_without_source_files" : "embedded_source_available";
        } else if (parts.contains("ILDB") || parts.contains("SRCI"))
            result["status"] = "unsupported_debug_format";
    } catch (const std::exception &e) {
        result["status"] = "invalid_embedded_source";
        result["files"] = Json::array();
        result["environment"] = Json::object();
        result["issues"].push_back(e.what());
    }
    return result;
}
} // namespace flora::shader_debug
