#include "SourceVariables.h"
#include "ShaderDebugData.h"
#include "core/Dxbc.h"
#include <bit>
#include <charconv>
#include <cmath>

namespace flora {
namespace {
using Json = nlohmann::json;
using namespace shader_debug;
template <class T = uint32_t> T at(Bytes bytes, size_t offset) {
    return Reader(sub(bytes, offset, sizeof(T))).read<T>();
}
std::pair<std::string, size_t> text(Bytes bytes, size_t offset) {
    auto [value, end] = stringAt(bytes, offset);
    return {strictDebugText(value), end};
}
std::string hex(uint64_t value) {
    char buffer[32];
    auto result = std::to_chars(buffer, buffer + sizeof buffer, value, 16);
    return "0x" + std::string(buffer, result.ptr);
}
void appendChild(Json &leaves, Json &vectors, const Json &child, const std::string &prefix, uint64_t offset) {
    for (auto row : child.at("leaves")) {
        row["path"] = prefix + row.at("path").get<std::string>();
        row["offset"] = offset + row.at("offset").get<uint64_t>();
        leaves.push_back(std::move(row));
    }
    for (auto row : child.value("vectors", Json::array())) {
        row["path"] = prefix + row.at("path").get<std::string>();
        for (auto &member : row["members"])
            member = prefix + member.get<std::string>();
        vectors.push_back(std::move(row));
    }
}
} // namespace
namespace codeview {
std::pair<uint64_t, size_t> numeric(Bytes bytes, size_t offset) {
    Reader r(sub(bytes, offset, bytes.size() - std::min(offset, bytes.size())));
    const auto tag = r.read<uint16_t>();
    uint64_t value;
    if (tag < 0x8000)
        value = tag;
    else {
        auto signedValue = [&]<class T>() {
            auto n = r.read<T>();
            if (n < 0)
                throw std::runtime_error("Negative CodeView size or offset");
            return uint64_t(n);
        };
        switch (tag) {
        case 0x8000:
            value = signedValue.template operator()<int8_t>();
            break;
        case 0x8001:
            value = signedValue.template operator()<int16_t>();
            break;
        case 0x8002:
            value = r.read<uint16_t>();
            break;
        case 0x8003:
            value = signedValue.template operator()<int32_t>();
            break;
        case 0x8004:
            value = r.read<uint32_t>();
            break;
        case 0x8009:
            value = signedValue.template operator()<int64_t>();
            break;
        case 0x800a:
            value = r.read<uint64_t>();
            break;
        default:
            throw std::runtime_error("Unsupported CodeView numeric leaf");
        }
    }
    return {value, offset + r.position()};
}
Records records(Bytes data) {
    if (data.size() < 56)
        throw std::runtime_error("Truncated CodeView type stream");
    Reader h(data);
    const auto version = h.read<uint32_t>(), head = h.read<uint32_t>(), first = h.read<uint32_t>(),
               last = h.read<uint32_t>(), size = h.read<uint32_t>();
    if (version != 20040203 || head < 56 || uint64_t(head) + size > data.size() || first < 0x1000 ||
        first > last || last > 0x110000)
        throw std::runtime_error("CodeView type stream bounds");
    Reader r(sub(data, head, size));
    Records result;
    for (auto index = first; index < last; ++index) {
        if (r.remaining() < 4)
            throw std::runtime_error("Truncated CodeView type record");
        const auto length = r.read<uint16_t>();
        if (length < 2 || length > r.remaining())
            throw std::runtime_error("CodeView type record bounds");
        Reader item(r.take(length));
        const auto kind = item.read<uint16_t>();
        const auto payload = item.take(item.remaining());
        result.emplace(index, std::pair{kind, std::vector<uint8_t>(payload.begin(), payload.end())});
    }
    if (r.remaining())
        throw std::runtime_error("CodeView type count mismatch");
    return result;
}
Json Types::get(uint32_t index, std::vector<uint32_t> parents) {
    if (cache_.contains(index))
        return cache_.at(index);
    if (parents.size() > 32 || std::find(parents.begin(), parents.end(), index) != parents.end())
        throw std::runtime_error("Recursive source value type");
    static const std::map<uint32_t, std::pair<const char *, uint32_t>> primitives{
        {0x40, {"float", 4}}, {0x41, {"double", 8}}, {0x74, {"int", 4}},  {0x75, {"uint", 4}},
        {0x76, {"int64", 8}}, {0x77, {"uint64", 8}}, {0x32, {"bool", 4}}, {0x62, {"bool", 4}}};
    if (auto found = primitives.find(index); found != primitives.end()) {
        const auto &[name, size] = found->second;
        return {{"name", name},
                {"size", size},
                {"leaves", Json::array({{{"path", ""}, {"offset", 0}, {"type", name}, {"size", size}}})}};
    }
    if (index < 0x1000)
        throw std::runtime_error("Unsupported primitive source type " + hex(index));
    if (!records_.contains(index))
        throw std::runtime_error("Missing CodeView type record " + hex(index));
    const auto &[kind, storage] = records_.at(index);
    const Bytes data(storage);
    parents.push_back(index);
    Json result;
    if (kind == 0x1001 || kind == 0x1518)
        result = get(at(data, 0), parents);
    else if (kind == 0x151b || kind == 0x1503 || kind == 0x1516) {
        const auto element = at(data, 0), second = at(data, 4);
        const auto child = get(element, parents);
        const auto [size, end] = numeric(data, kind == 0x1516 ? 12 : 8);
        const auto childSize = child.at("size").get<uint64_t>();
        const uint64_t stride = kind == 0x1516 ? at(data, 8) : childSize;
        const uint64_t count = kind == 0x151b                ? second
                               : stride && size >= childSize ? (size - childSize) / stride + 1
                                                             : 0;
        if (count < 1 || count > 4096 || count * child.at("leaves").size() > 4096 || stride < childSize ||
            (count - 1) * stride + childSize != size)
            throw std::runtime_error("Invalid vector or array size");
        auto name = text(data, end).first;
        if (name.empty())
            name = child.at("name").get<std::string>() + "[" + std::to_string(count) + "]";
        Json leaves = Json::array(), vectors = Json::array();
        for (uint64_t i = 0; i < count; ++i) {
            const auto suffix = kind == 0x151b && count <= 4 ? "." + std::string(1, "xyzw"[i])
                                                             : "[" + std::to_string(i) + "]";
            appendChild(leaves, vectors, child, suffix, i * stride);
        }
        if (kind == 0x151b && count <= 4 && child.at("leaves").size() == 1) {
            Json members = Json::array();
            for (uint64_t i = 0; i < count; ++i)
                members.push_back("." + std::string(1, "xyzw"[i]));
            vectors.push_back({{"path", ""}, {"members", members}});
        }
        result = {{"name", name},
                  {"size", size},
                  {"leaves", leaves},
                  {"array", kind != 0x151b},
                  {"vectors", vectors}};
    } else if (kind == 0x151c) {
        const auto element = at(data, 0), rows = at(data, 4), cols = at(data, 8), stride = at(data, 12);
        const auto flags = at<uint8_t>(data, 16);
        const auto [size, pos] = numeric(data, 17);
        const auto name = text(data, pos).first;
        const auto child = get(element, parents);
        if ((flags & ~1) || rows < 1 || rows > 4 || cols < 1 || cols > 4 || child.at("leaves").size() != 1)
            throw std::runtime_error("Unsupported source matrix layout");
        Json leaves = Json::array(), vectors = Json::array();
        for (uint32_t r = 0; r < rows; ++r) {
            Json members = Json::array();
            const auto prefix = "[" + std::to_string(r) + "]";
            for (uint32_t c = 0; c < cols; ++c) {
                const uint64_t offset = uint64_t((flags & 1) ? r : c) * stride +
                                        ((flags & 1) ? c : r) * child.at("size").get<uint64_t>();
                if (offset + child.at("size").get<uint64_t>() > size)
                    throw std::runtime_error("Source matrix member bounds");
                auto leaf = child.at("leaves").at(0);
                const auto path = prefix + "[" + std::to_string(c) + "]";
                leaf.update({{"path", path}, {"offset", offset}});
                leaves.push_back(leaf);
                members.push_back(path);
            }
            vectors.push_back({{"path", prefix}, {"members", members}});
        }
        result = {{"name", name}, {"size", size}, {"leaves", leaves}, {"vectors", vectors}};
    } else if (kind == 0x1504 || kind == 0x1505) {
        const auto count = at<uint16_t>(data, 0), flags = at<uint16_t>(data, 2);
        const auto fields = at(data, 4), derived = at(data, 8), vshape = at(data, 12);
        const auto [size, nameAt] = numeric(data, 16);
        const auto name = text(data, nameAt).first;
        if (count > 4096 || (flags & 0x80) || derived || vshape)
            throw std::runtime_error("Incomplete or inherited source structure");
        if (!records_.contains(fields) || records_.at(fields).first != 0x1203)
            throw std::runtime_error("Missing structure field list");
        const Bytes members(records_.at(fields).second);
        size_t pos = 0;
        Json leaves = Json::array(), vectors = Json::array();
        for (unsigned i = 0; i < count; ++i) {
            while (pos < members.size() && members[pos] >= 0xf0) {
                const auto padding = members[pos] & 15;
                if (!padding || size_t(padding) > members.size() - pos)
                    throw std::runtime_error("Invalid field padding");
                pos += padding;
            }
            const auto member = at<uint16_t>(members, pos), attr = at<uint16_t>(members, pos + 2);
            (void)attr;
            const auto type = at(members, pos + 4);
            if (member != 0x150d)
                throw std::runtime_error("Unsupported structure field");
            const auto [offset, labelAt] = numeric(members, pos + 8);
            const auto [label, end] = text(members, labelAt);
            pos = end;
            const auto child = get(type, parents);
            if (offset > size || child.at("size").get<uint64_t>() > size - offset)
                throw std::runtime_error("Structure member outside value");
            appendChild(leaves, vectors, child, "." + label, offset);
            if (leaves.size() > 4096)
                throw std::runtime_error("Too many structure components");
        }
        result = {{"name", name}, {"size", size}, {"leaves", leaves}, {"vectors", vectors}};
    } else
        throw std::runtime_error("Unsupported source type leaf " + hex(kind));
    if (result.at("size").get<uint64_t>() > 65536 || result.at("leaves").size() > 4096)
        throw std::runtime_error("Source value exceeds inspection limit");
    cache_[index] = result;
    return result;
}
} // namespace codeview

Json sourceVariables(Bytes raw) {
    Json result{{"status", "unavailable"},
                {"shader_sha256", sha256(raw)},
                {"variables", Json::array()},
                {"scopes", Json::array()},
                {"issues", Json::array()}};
    const auto parts = shader_debug::chunks(raw);
    if (!parts.contains("SPDB")) {
        if (parts.contains("SDBG"))
            result["status"] = "pending_native_sdbg_symbols";
        return result;
    }
    try {
        shader_debug::Pdb pdb(parts.at("SPDB"));
        codeview::Types types(codeview::records(pdb.stream(2)));
        const auto ids = codeview::records(pdb.stream(4));
        const auto code = parts.at(parts.contains("SHEX") ? "SHEX" : "SHDR");
        const auto program = readDxbcProgram(code);
        std::set<uint64_t> boundaries{code.size()};
        uint64_t offset = 8;
        for (const auto &op : program.instructions) {
            boundaries.insert(offset);
            offset += op.size() * 4;
        }
        const auto dbi = pdb.stream(3);
        if (dbi.size() < 64 || at(dbi, 0) != UINT32_MAX || at(dbi, 4) != 19990903)
            throw std::runtime_error("Unsupported SPDB DBI");
        const auto modules = sub(dbi, 64, at(dbi, 24));
        size_t pos = 0;
        while (pos < modules.size()) {
            const auto module = sub(modules, pos, 64);
            const auto stream = at<uint16_t>(module, 34);
            const auto size = at(module, 36);
            auto end = stringAt(modules, pos + 64).second;
            end = stringAt(modules, end).second;
            pos = (end + 3) & ~size_t(3);
            if (pos > modules.size())
                throw std::runtime_error("Module alignment bounds");
            if (stream == 0xffff)
                continue;
            const auto data = pdb.stream(stream);
            if (size < 4 || size > data.size() || at(data, 0) != 4)
                throw std::runtime_error("Symbol stream bounds");
            std::map<uint32_t, std::pair<uint16_t, Bytes>> symbols;
            size_t cur = 4;
            while (cur < size) {
                const auto length = at<uint16_t>(data, cur), kind = at<uint16_t>(data, cur + 2);
                const auto stop = cur + length + 2;
                if (length < 2 || stop > size)
                    throw std::runtime_error("Symbol record bounds");
                symbols[uint32_t(cur)] = {kind, sub(data, cur + 4, length - 2)};
                cur = stop;
            }
            std::vector<Json> scopes;
            std::optional<size_t> local;
            for (const auto &[address, symbol] : symbols) {
                const auto &[kind, payload] = symbol;
                if (kind == 0x110f || kind == 0x1110 || kind == 0x1146 || kind == 0x1147 || kind == 0x1103 ||
                    kind == 0x114d || kind == 0x115d) {
                    const auto parent = at(payload, 0), stop = at(payload, 4);
                    if (!symbols.contains(stop) || stop <= address ||
                        parent != (scopes.empty() ? 0u : scopes.back().at("offset").get<uint32_t>()))
                        throw std::runtime_error("Inconsistent source symbol scope");
                    Json scope{{"id", std::to_string(stream) + ":" + std::to_string(address)},
                               {"offset", address},
                               {"end", stop},
                               {"parent", scopes.empty() ? Json(nullptr) : scopes.back().at("id")}};
                    if (kind == 0x114d || kind == 0x115d) {
                        const auto id = at(payload, 8);
                        if (!ids.contains(id) || (ids.at(id).first != 0x1601 && ids.at(id).first != 0x1602))
                            throw std::runtime_error("Unsupported inline function identity");
                        const auto begin = kind == 0x115d ? 16u : 12u;
                        const auto annotations =
                            sub(payload, begin, payload.size() - std::min<size_t>(begin, payload.size()));
                        scope.update(
                            {{"kind", "inline"},
                             {"name", text(ids.at(id).second, 8).first},
                             {"inlinee", id},
                             {"annotations", QByteArray(reinterpret_cast<const char *>(annotations.data()),
                                                        qsizetype(annotations.size()))
                                                 .toHex()
                                                 .toStdString()}});
                    } else {
                        const auto start = at(payload, kind == 0x1103 ? 12 : 28),
                                   length = at(payload, kind == 0x1103 ? 8 : 12);
                        const auto segment = at<uint16_t>(payload, kind == 0x1103 ? 16 : 32);
                        if (segment != 1 || !boundaries.contains(start) ||
                            !boundaries.contains(uint64_t(start) + length))
                            throw std::runtime_error("Source scope code bounds");
                        scope.update({{"kind", kind == 0x1103 ? "block" : "function"},
                                      {"name", text(payload, kind == 0x1103 ? 18 : 35).first},
                                      {"code_start", start},
                                      {"code_end", uint64_t(start) + length}});
                    }
                    result["scopes"].push_back(scope);
                    scopes.push_back(scope);
                    local.reset();
                    if (result["scopes"].size() > 4096 || scopes.size() > 64)
                        throw std::runtime_error("Source scope inspection limit");
                } else if (kind == 6 || kind == 0x114e || kind == 0x114f) {
                    if (scopes.empty() || scopes.back().at("end") != address)
                        throw std::runtime_error("Unmatched source scope end");
                    scopes.pop_back();
                    local.reset();
                } else if (kind == 0x113e) {
                    const auto index = at(payload, 0);
                    const auto flags = at<uint16_t>(payload, 4);
                    Json variable{{"id", std::to_string(stream) + ":" + std::to_string(address)},
                                  {"name", text(payload, 6).first},
                                  {"type_index", index},
                                  {"flags", flags},
                                  {"scope", scopes.empty() ? Json(nullptr) : scopes.back().at("id")},
                                  {"ranges", Json::array()}};
                    try {
                        variable["type"] = types.get(index);
                    } catch (const std::exception &e) {
                        variable["type_issue"] = e.what();
                    }
                    local = result["variables"].size();
                    result["variables"].push_back(variable);
                    if (result["variables"].size() > 4096)
                        throw std::runtime_error("Too many source local symbols");
                } else if (kind == 0x1150) {
                    if (!local)
                        throw std::runtime_error("HLSL range without a consecutive local");
                    auto &variable = result["variables"].at(*local);
                    const auto reg = at<uint16_t>(payload, 0), flags = at<uint16_t>(payload, 2),
                               parent = at<uint16_t>(payload, 4), parentSize = at<uint16_t>(payload, 6),
                               segment = at<uint16_t>(payload, 12), length = at<uint16_t>(payload, 14);
                    const auto start = at(payload, 8);
                    const auto dimensions = flags & 3;
                    const auto tail = sub(payload, 16, payload.size() - std::min<size_t>(16, payload.size()));
                    if (tail.size() < 4u * dimensions)
                        throw std::runtime_error("HLSL live range bounds");
                    const auto gapBytes = tail.size() - 4u * dimensions;
                    if (gapBytes % 4 || segment != 1 || !boundaries.contains(start) ||
                        !boundaries.contains(uint64_t(start) + length))
                        throw std::runtime_error("HLSL live range bounds");
                    Json gaps = Json::array(), indices = Json::array();
                    for (size_t i = 0; i < gapBytes; i += 4) {
                        const auto a = at<uint16_t>(tail, i), b = at<uint16_t>(tail, i + 2);
                        if (uint32_t(a) + b > length || !boundaries.contains(uint64_t(start) + a) ||
                            !boundaries.contains(uint64_t(start) + a + b))
                            throw std::runtime_error("HLSL live gap bounds");
                        gaps.push_back({a, b});
                    }
                    if (variable.contains("type") &&
                        uint32_t(parent) + parentSize > variable.at("type").at("size").get<uint64_t>())
                        throw std::runtime_error("HLSL range outside source value");
                    for (int i = 0; i < dimensions; ++i)
                        indices.push_back(at(tail, gapBytes + i * 4));
                    variable["ranges"].push_back({{"register_type", reg},
                                                  {"flags", flags},
                                                  {"variable_offset", parent},
                                                  {"size", parentSize},
                                                  {"start", start},
                                                  {"end", uint64_t(start) + length},
                                                  {"gaps", gaps},
                                                  {"register_offsets", indices}});
                } else
                    local.reset();
            }
            if (!scopes.empty())
                throw std::runtime_error("Unclosed source scope");
        }
        size_t total = 0;
        for (const auto &v : result["variables"])
            if (v.contains("type"))
                total += v.at("type").at("leaves").size();
        if (total > 65536)
            throw std::runtime_error("Too many source variable components");
        result["status"] = result["variables"].empty() ? "no_local_symbols" : "available";
    } catch (const std::exception &e) {
        result["status"] = "invalid_or_unsupported_symbols";
        result["variables"] = Json::array();
        result["scopes"] = Json::array();
        result["issues"].push_back(e.what());
    }
    return result;
}
namespace {
std::string floatValue(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    char buffer[64];
    auto converted = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    if (converted.ec != std::errc{})
        throw std::runtime_error("Cannot format source variable value");
    std::string repr(buffer, converted.ptr);
    const auto e = repr.find('e');
    const int exponent = std::stoi(repr.substr(e + 1));
    if (exponent < -4 || exponent >= 16)
        return repr;
    const bool negative = repr[0] == '-';
    auto digits = repr.substr(negative ? 1 : 0, e - (negative ? 1 : 0));
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    const int point = exponent + 1;
    std::string result = negative ? "-" : "";
    if (point <= 0)
        return result + "0." + std::string(size_t(-point), '0') + digits;
    if (size_t(point) >= digits.size())
        return result + digits + std::string(size_t(point) - digits.size(), '0') + ".0";
    return result + digits.substr(0, point) + "." + digits.substr(point);
}
} // namespace
Json resolveSourceVariables(const Json &model, const Json &registers, const Json &meta, const Json &row,
                            uint64_t byteOffset) {
    if (model.value("format", "") == "SDBG assignments" ||
        model.value("status", "") == "pending_native_sdbg_symbols")
        throw std::runtime_error("Native SDBG assignment reconstruction is pending");
    std::map<std::string, Json> byName, scopes;
    for (const auto &reg : registers)
        byName[reg.at("name").get<std::string>()] = reg;
    for (const auto &scope : model.at("scopes"))
        scopes[scope.at("id").get<std::string>()] = scope;
    std::map<uint32_t, Json> arrays;
    for (const auto &array : meta.value("indexable_temporaries", Json::array()))
        arrays[array.at("array").get<uint32_t>()] = array;
    const auto stage = meta.value("shader_stage", "");
    std::optional<std::set<std::string>> allowed;
    if (stage == "hs") {
        allowed.emplace();
        Json phases = Json::array();
        for (const auto &phase : model.value("hs_phases", Json::array()))
            if (phase.at("id") == meta.at("hs_phase").at("id") &&
                phase.at("start").get<uint64_t>() <= byteOffset &&
                byteOffset < phase.at("end").get<uint64_t>())
                phases.push_back(phase);
        if (phases.size() == 1)
            for (const auto &id : phases.at(0).at("scope_ids"))
                allowed->insert(id.get<std::string>());
    }
    using Value = std::optional<std::pair<uint32_t, std::string>>;
    auto component = [&](const Json &binding, uint64_t offset) -> Value {
        const auto kind = binding.at("register_type").get<uint32_t>();
        const auto &indices = binding.at("register_offsets");
        const auto parent = binding.at("variable_offset").get<uint64_t>();
        if (offset < parent || (binding.at("flags").get<uint32_t>() & ~7))
            return {};
        const auto relative = offset - parent;
        const auto depth = row.value("call_depth", 0u);
        if (kind == 11 || kind == 37) {
            if (depth || relative || offset % 4)
                return {};
            const auto value = row.value(kind == 11 ? "primitive_id" : "gs_instance", Json(nullptr));
            if (value.is_null())
                return {};
            return std::pair{value.get<uint32_t>(), std::string(kind == 11 ? "vPrim" : "vGSInstanceID")};
        }
        if (kind == 22 && stage == "hs") {
            if (indices != Json::array({0xffffffd0u}) || relative || offset % 4 ||
                meta.at("hs_phase").at("kind") != "control_points")
                return {};
            auto found = byName.find("vOutputControlPointID");
            if (found == byName.end() || found->second.at("written").at(0) != true)
                return {};
            return std::pair{found->second.at("bits").at(0).get<uint32_t>(),
                             std::string("vOutputControlPointID.x")};
        }
        std::string name;
        uint64_t part = 0, c = 0;
        if (kind == 3 && indices.size() == 1 && arrays.contains(indices.at(0).get<uint32_t>())) {
            const auto id = indices.at(0).get<uint32_t>();
            const auto &array = arrays.at(id);
            const auto element = relative / 16;
            part = relative % 16;
            c = part / 4;
            name = "x" + std::to_string(id) + "[" + std::to_string(element) + "]";
            if (element >= array.at("elements").get<uint64_t>() ||
                c >= array.at("components").get<uint32_t>())
                return {};
        } else if ((kind == 25 || kind == 26 || kind == 27 || kind == 28) &&
                   (stage == "ds" || stage == "hs")) {
            if (depth)
                return {};
            if ((kind == 25 || kind == 26) && indices.size() == 2 && (kind == 25 || stage == "hs")) {
                const auto address = indices.at(0).get<uint32_t>() + relative;
                part = address % 16;
                name = std::string(kind == 25 ? "vicp" : "vocp") + "[" +
                       std::to_string(indices.at(1).get<uint32_t>()) + "][" + std::to_string(address / 16) +
                       "]";
            } else if (kind == 27 && indices.size() == 1) {
                const auto address = indices.at(0).get<uint32_t>() + relative;
                part = address % 16;
                name = "vpc" + std::to_string(address / 16);
            } else if (kind == 28 && stage == "ds" && indices.size() == 1 &&
                       indices.at(0).get<uint32_t>() >= 0xffffffc0u &&
                       indices.at(0).get<uint32_t>() <= 0xffffffc8u) {
                part = indices.at(0).get<uint32_t>() - 0xffffffc0u + relative;
                name = "vDomain";
            } else
                return {};
            c = part / 4;
            if (c >= 4)
                return {};
        } else if (kind <= 2 && indices.size() == (kind == 1 ? 2u : 1u)) {
            const auto address = indices.at(0).get<uint32_t>() + relative;
            part = address % 16;
            c = part / 4;
            name = kind == 1 ? "v[" + std::to_string(indices.at(1).get<uint32_t>()) + "][" +
                                   std::to_string(address / 16) + "]"
                             : std::string(kind == 0 ? "r" : "o") + std::to_string(address / 16);
        } else
            return {};
        const auto found = byName.find(name);
        if (part % 4 || found == byName.end() || c >= 4 || found->second.at("written").at(size_t(c)) != true)
            return {};
        return std::pair{found->second.at("bits").at(size_t(c)).get<uint32_t>(), name + "." + "xyzw"[c]};
    };
    Json result = Json::array();
    for (const auto &variable : model.at("variables")) {
        const auto scopeId = variable.at("scope");
        if (allowed && (!scopeId.is_string() || !allowed->contains(scopeId.get<std::string>())))
            continue;
        const auto found = scopeId.is_string() ? scopes.find(scopeId.get<std::string>()) : scopes.end();
        const Json scope = found == scopes.end() ? Json::object() : found->second;
        auto label = scope.value("name", "");
        if (scope.value("kind", "") == "inline" || scope.value("kind", "") == "block") {
            size_t i = 0;
            for (const auto &entry : model.at("scopes")) {
                ++i;
                if (entry.at("id") == scopeId)
                    break;
            }
            label = (label.empty() ? "块" : label) + std::string(" #") + std::to_string(i);
        }
        Json base{{"variable_id", variable.at("id")},
                  {"scope_id", scopeId},
                  {"scope", scope.value("name", "")},
                  {"scope_label", label},
                  {"name", variable.at("name")}};
        if (!variable.contains("type")) {
            base.update({{"type", "unknown"},
                         {"status", "unsupported_type"},
                         {"value", nullptr},
                         {"bits", nullptr},
                         {"references", Json::array()},
                         {"issue", variable.value("type_issue", "")}});
            result.push_back(base);
            continue;
        }
        const auto &type = variable.at("type");
        for (const auto &leaf : type.at("leaves")) {
            auto item = base;
            item.update(
                {{"name", variable.at("name").get<std::string>() + leaf.at("path").get<std::string>()},
                 {"type", leaf.at("type")},
                 {"status", "unavailable"},
                 {"value", nullptr},
                 {"bits", nullptr},
                 {"references", Json::array()}});
            const auto size = leaf.at("size").get<uint64_t>(), start = leaf.at("offset").get<uint64_t>();
            if (size != 4 && size != 8) {
                item["status"] = "unsupported_width";
                result.push_back(item);
                continue;
            }
            std::vector<uint32_t> words;
            std::set<std::string> refs;
            for (uint64_t part = 0; part < size; part += 4) {
                if (start > UINT64_MAX - part - 4)
                    throw std::runtime_error("Source variable offset overflow");
                const auto offset = start + part;
                std::vector<Value> values;
                for (const auto &binding : variable.at("ranges")) {
                    if (binding.at("register_type") == 3) {
                        const auto &indices = binding.at("register_offsets");
                        const auto array =
                            indices.size() == 1 ? arrays.find(indices.at(0).get<uint32_t>()) : arrays.end();
                        if (!type.value("array", false) || array == arrays.end() ||
                            binding.at("variable_offset") != 0 ||
                            type.at("size").get<uint64_t>() >
                                array->second.at("elements").get<uint64_t>() * 16)
                            continue;
                    }
                    const auto begin = binding.at("start").get<uint64_t>(),
                               end = binding.at("end").get<uint64_t>(),
                               parent = binding.at("variable_offset").get<uint64_t>(),
                               width = binding.at("size").get<uint64_t>();
                    bool gap = false;
                    for (const auto &g : binding.at("gaps")) {
                        const auto a = g.at(0).get<uint64_t>(), b = g.at(1).get<uint64_t>();
                        if (byteOffset >= begin && byteOffset - begin >= a && byteOffset - begin - a < b)
                            gap = true;
                    }
                    if (begin <= byteOffset && byteOffset < end && !gap && parent <= offset &&
                        offset - parent <= width && width - (offset - parent) >= 4)
                        values.push_back(component(binding, offset));
                }
                if (values.empty() ||
                    std::any_of(values.begin(), values.end(), [](const auto &v) { return !v; }))
                    break;
                if (std::any_of(values.begin(), values.end(),
                                [&](const auto &v) { return v->first != values[0]->first; })) {
                    item["status"] = "ambiguous";
                    break;
                }
                words.push_back(values[0]->first);
                for (const auto &v : values)
                    refs.insert(v->second);
            }
            if (words.size() * 4 == size) {
                const uint64_t bits = uint64_t(words[0]) | (size == 8 ? uint64_t(words[1]) << 32 : 0);
                const auto kind = leaf.at("type").get<std::string>();
                std::string value;
                if (kind == "float")
                    value = floatValue(std::bit_cast<float>(uint32_t(bits)));
                else if (kind == "double")
                    value = floatValue(std::bit_cast<double>(bits));
                else if (kind == "int")
                    value = std::to_string(std::bit_cast<int32_t>(uint32_t(bits)));
                else if (kind == "int64")
                    value = std::to_string(std::bit_cast<int64_t>(bits));
                else if (kind == "bool")
                    value = bits ? "True" : "False";
                else
                    value = std::to_string(bits);
                item.update(
                    {{"status", "available"}, {"value", value}, {"bits", bits}, {"references", refs}});
            }
            result.push_back(std::move(item));
        }
    }
    return result;
}
} // namespace flora
