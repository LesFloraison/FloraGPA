#include "ShaderInspector.h"
#include "replay/Replay.h"
#include <QByteArray>
#include <QStringDecoder>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
constexpr size_t limit = 128 * 1024 * 1024;
Bytes sub(Bytes b, size_t offset, size_t size) {
    if (offset > b.size() || size > b.size() - offset)
        throw std::runtime_error("Shader data range exceeds storage");
    return b.subspan(offset, size);
}
std::pair<std::string, size_t> stringAt(Bytes bytes, size_t pos) {
    if (pos >= bytes.size())
        throw std::runtime_error("Shader string offset exceeds storage");
    auto end = std::find(bytes.begin() + pos, bytes.end(), uint8_t(0));
    if (end == bytes.end())
        throw std::runtime_error("Unterminated shader string");
    return {{reinterpret_cast<const char *>(bytes.data() + pos), size_t(end - (bytes.begin() + pos))},
            size_t(end - bytes.begin()) + 1};
}
std::map<std::string, Bytes> chunks(Bytes bytes) {
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
    for (UINT i = 0; i < count; ++i) {
        auto offset = r.read<uint32_t>();
        if (offset < 32 + count * 4)
            throw std::runtime_error("DXBC chunk overlaps header");
        Reader chunk(sub(bytes, offset, bytes.size() - std::min<size_t>(offset, bytes.size())));
        auto tag = chunk.take(4);
        std::string name(reinterpret_cast<const char *>(tag.data()), 4);
        auto data = chunk.take(chunk.read<UINT>());
        if (!result.emplace(name, data).second)
            throw std::runtime_error("Duplicate DXBC chunk");
    }
    return result;
}
Json sourceFile(const std::string &name, Bytes bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8);
    std::string encoding = "utf-8";
    if (bytes.size() >= 2 &&
        ((bytes[0] == 0xff && bytes[1] == 0xfe) || (bytes[0] == 0xfe && bytes[1] == 0xff))) {
        decoder = QStringDecoder(QStringDecoder::Utf16);
        encoding = "utf-16";
    } else if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
        encoding = "utf-8-sig";
    QString text =
        decoder(QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
    return {{"name", QString::fromUtf8(name).toStdString()},
            {"byte_length", bytes.size()},
            {"sha256", sha256(bytes)},
            {"encoding", encoding},
            {"text_valid", !decoder.hasError()},
            {"text", text.toStdString()},
            {"raw_hex", QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                            .toHex()
                            .toStdString()}};
}
class Pdb {
    Bytes data_;
    UINT page_{}, count_{};
    struct Stream {
        std::vector<UINT> pages;
        UINT size;
    };
    std::vector<Stream> streams_;
    Bytes block(UINT id) const {
        if (id >= count_)
            throw std::runtime_error("SPDB page outside file");
        return sub(data_, size_t(id) * page_, page_);
    }
    std::vector<uint8_t> collect(const std::vector<UINT> &ids, UINT size) const {
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
        page_ = r.read<UINT>();
        auto free = r.read<UINT>();
        count_ = r.read<UINT>();
        auto size = r.read<UINT>(), reserved = r.read<UINT>(), mapping = r.read<UINT>();
        if ((page_ != 512 && page_ != 1024 && page_ != 2048 && page_ != 4096) ||
            uint64_t(count_) * page_ != bytes.size() || (free != 1 && free != 2) || reserved)
            throw std::runtime_error("SPDB superblock invalid");
        auto blocks = (uint64_t(size) + page_ - 1) / page_;
        if (size > bytes.size() || blocks * 4 > page_)
            throw std::runtime_error("SPDB directory exceeds limit");
        Reader map(block(mapping));
        std::vector<UINT> indices;
        for (size_t i = 0; i < blocks; ++i)
            indices.push_back(map.read<UINT>());
        auto raw = collect(indices, size);
        Reader directory(raw);
        auto count = directory.read<UINT>();
        if (count > 65536)
            throw std::runtime_error("SPDB stream count limit");
        std::vector<UINT> sizes;
        for (UINT i = 0; i < count; ++i)
            sizes.push_back(directory.read<UINT>());
        for (auto n : sizes) {
            Stream stream{{}, n};
            if (n != UINT_MAX) {
                if (n > bytes.size())
                    throw std::runtime_error("SPDB stream exceeds file");
                auto pages = (uint64_t(n) + page_ - 1) / page_;
                for (size_t i = 0; i < pages; ++i) {
                    auto id = directory.read<UINT>();
                    block(id);
                    stream.pages.push_back(id);
                }
            }
            streams_.push_back(std::move(stream));
        }
        directory.end();
    }
    std::vector<uint8_t> stream(UINT id) const {
        if (id >= streams_.size() || streams_[id].size == UINT_MAX)
            throw std::runtime_error("Missing SPDB stream");
        return collect(streams_[id].pages, streams_[id].size);
    }
    std::map<std::string, UINT> names() const {
        auto bytes = stream(1);
        Reader r(bytes);
        if (r.read<UINT>() != 20000404)
            throw std::runtime_error("Unsupported SPDB named stream version");
        r.skip(24);
        auto strings = r.take(r.read<UINT>());
        auto count = r.read<UINT>(), capacity = r.read<UINT>();
        if (count > capacity || capacity > 1048576)
            throw std::runtime_error("Invalid SPDB hash capacity");
        auto bits = [&] {
            auto words = r.read<UINT>();
            if (words > (capacity + 31) / 32)
                throw std::runtime_error("SPDB occupancy limit");
            std::set<UINT> result;
            for (UINT i = 0; i < words; ++i) {
                auto word = r.read<UINT>();
                for (UINT b = 0; b < 32; ++b)
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
        std::map<std::string, UINT> result;
        for (auto index : present) {
            (void)index;
            auto offset = r.read<UINT>(), id = r.read<UINT>();
            auto name = stringAt(strings, offset).first;
            if (id >= streams_.size() || streams_[id].size == UINT_MAX || !result.emplace(name, id).second)
                throw std::runtime_error("Invalid SPDB named stream");
        }
        return result;
    }
    Json environment() const {
        auto dbi = stream(3);
        Reader head(dbi);
        if (head.read<UINT>() != UINT_MAX || head.read<UINT>() != 19990903 || dbi.size() < 64)
            throw std::runtime_error("Unsupported SPDB DBI header");
        head.skip(16);
        auto size = head.read<UINT>();
        auto modules = sub(dbi, 64, size);
        size_t pos = 0;
        Json result = Json::object();
        while (pos < modules.size()) {
            Reader module(sub(modules, pos, 64));
            module.skip(34);
            auto id = module.read<uint16_t>();
            auto symbolSize = module.read<UINT>();
            auto end = stringAt(modules, pos + 64).second;
            end = stringAt(modules, end).second;
            pos = (end + 3) & ~size_t(3);
            if (pos > modules.size())
                throw std::runtime_error("SPDB module alignment bounds");
            if (id == 0xffff)
                continue;
            auto data = stream(id);
            Reader symbols(sub(data, 0, symbolSize));
            if (symbols.read<UINT>() != 4)
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
Json embedded(const std::map<std::string, Bytes> &parts) {
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
            auto h = r.array<UINT, 21>();
            if (h[0] != 84)
                throw std::runtime_error("Unsupported SDBG header");
            auto base = data.subspan(84);
            auto ints = h[19], strings = h[20];
            if (ints > strings || strings > base.size() || ints % 4 || (strings - ints) % 4)
                throw std::runtime_error("SDBG datastore bounds");
            const UINT strides[] = {16, 396, 24, 24, 20, 20, 44};
            std::vector<std::pair<UINT, UINT>> intervals;
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
            for (UINT i = 0; i < h[5]; ++i) {
                auto name = files.read<UINT>(), length = files.read<UINT>(), source = files.read<UINT>(),
                     size = files.read<UINT>();
                auto n = sub(ascii, name, length);
                auto bytes = sub(ascii, source, size);
                total += size;
                if (total > limit)
                    throw std::runtime_error("SDBG source size limit");
                auto item = sourceFile({reinterpret_cast<const char *>(n.data()), n.size()}, bytes);
                item["sdbg_file"] = i;
                result["files"].push_back(item);
            }
            result["environment"] = {{"hlslEntry", stringAt(ascii, h[2]).first},
                                     {"hlslTarget", stringAt(ascii, h[3]).first},
                                     {"hlslFlags", h[4]},
                                     {"hlslCompiler", stringAt(ascii, h[1]).first}};
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
} // namespace
static Json reflectedType(ID3D11ShaderReflectionType *type, unsigned depth = 0) {
    if (depth > 64)
        throw std::runtime_error("Constant type nesting exceeds 64");
    D3D11_SHADER_TYPE_DESC t{};
    check(type->GetDesc(&t), "Reflect shader type");
    Json result{{"class_id", t.Class},  {"base_type", t.Type},    {"rows", t.Rows},
                {"columns", t.Columns}, {"elements", t.Elements}, {"members", t.Members}};
    if (t.Members) {
        result["member_types"] = Json::array();
        for (UINT i = 0; i < t.Members; ++i) {
            auto member = type->GetMemberTypeByIndex(i);
            D3D11_SHADER_TYPE_DESC desc{};
            check(member->GetDesc(&desc), "Reflect structure member");
            auto name = type->GetMemberTypeName(i);
            result["member_types"].push_back({{"name", name ? name : ""},
                                              {"offset", desc.Offset},
                                              {"type", reflectedType(member, depth + 1)}});
        }
    }
    return result;
}
Json inspectShader(Bytes bytes) {
    auto parts = chunks(bytes);
    Json result{{"sha256", sha256(bytes)},
                {"chunks", Json::object()},
                {"bindings", Json::array()},
                {"constant_buffers", Json::array()},
                {"signatures", Json::object()}};
    for (auto &[name, data] : parts)
        result["chunks"][name] = data.size();
    result["embedded_sources"] = embedded(parts);
    auto it = parts.find("SHEX");
    if (it == parts.end())
        it = parts.find("SHDR");
    if (it == parts.end()) {
        result["profile"] = nullptr;
        result["stage"] = "signature";
        return result;
    }
    Reader code(it->second);
    auto version = code.read<UINT>();
    if (uint64_t(code.read<UINT>()) * 4 != it->second.size())
        throw std::runtime_error("Shader program length mismatch");
    const std::string names[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
    auto type = version >> 16;
    if (type >= 6)
        throw std::runtime_error("Unknown shader program type");
    result["stage"] = names[type];
    result["profile"] =
        names[type] + "_" + std::to_string((version >> 4) & 15) + "_" + std::to_string(version & 15);
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(bytes.data(), bytes.size(), IID_ID3D11ShaderReflection, &reflection), "Reflect shader");
    D3D11_SHADER_DESC desc{};
    check(reflection->GetDesc(&desc), "Read shader descriptor");
    result["instructions"] = desc.InstructionCount;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        check(reflection->GetResourceBindingDesc(i, &b), "Reflect resource binding");
        result["bindings"].push_back({{"name", b.Name ? b.Name : ""},
                                      {"type", b.Type},
                                      {"slot", b.BindPoint},
                                      {"count", b.BindCount},
                                      {"dimension", b.Dimension},
                                      {"return_type", b.ReturnType}});
    }
    for (UINT i = 0; i < desc.ConstantBuffers; ++i) {
        auto buffer = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC b{};
        check(buffer->GetDesc(&b), "Reflect constant buffer");
        Json cb{{"name", b.Name ? b.Name : ""}, {"size", b.Size}, {"variables", Json::array()}};
        for (UINT j = 0; j < b.Variables; ++j) {
            auto variable = buffer->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC v{};
            check(variable->GetDesc(&v), "Reflect shader variable");
            D3D11_SHADER_TYPE_DESC t{};
            check(variable->GetType()->GetDesc(&t), "Reflect shader type");
            cb["variables"].push_back({{"name", v.Name ? v.Name : ""},
                                       {"offset", v.StartOffset},
                                       {"size", v.Size},
                                       {"flags", v.uFlags},
                                       {"type", t.Type},
                                       {"class", t.Class},
                                       {"rows", t.Rows},
                                       {"columns", t.Columns},
                                       {"elements", t.Elements},
                                       {"type_layout", reflectedType(variable->GetType())}});
        }
        result["constant_buffers"].push_back(cb);
    }
    for (int kind = 0; kind < 3; ++kind) {
        Json sig = Json::array();
        auto count = kind == 0   ? desc.InputParameters
                     : kind == 1 ? desc.OutputParameters
                                 : desc.PatchConstantParameters;
        for (UINT i = 0; i < count; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            check(kind == 0   ? reflection->GetInputParameterDesc(i, &p)
                  : kind == 1 ? reflection->GetOutputParameterDesc(i, &p)
                              : reflection->GetPatchConstantParameterDesc(i, &p),
                  "Reflect signature");
            sig.push_back({{"semantic", p.SemanticName ? p.SemanticName : ""},
                           {"index", p.SemanticIndex},
                           {"register", p.Register},
                           {"system_value", p.SystemValueType},
                           {"component_type", p.ComponentType},
                           {"mask", p.Mask},
                           {"stream", p.Stream}});
        }
        result["signatures"][kind == 0 ? "input" : kind == 1 ? "output" : "patch"] = sig;
    }
    return result;
}
} // namespace flora
