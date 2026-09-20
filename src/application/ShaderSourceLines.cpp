#include "ShaderSourceLines.h"
#include "ShaderDebugData.h"
#include "SystemDisassembly.h"
#include "UnicodeCaseFoldData.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <d3dcompiler.h>

namespace flora {
namespace {
using Json = nlohmann::json;
using namespace shader_debug;
template <class T = uint32_t> T at(Bytes bytes, size_t offset) {
    return Reader(sub(bytes, offset, sizeof(T))).read<T>();
}
std::string utf8(const std::string &raw) {
    const auto value =
        decodeSourceText(Bytes(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()), false);
    if (!value.valid)
        throw std::runtime_error("Invalid UTF-8 debug string");
    return value.text;
}
size_t lines(const Json &file) {
    const auto text = QString::fromStdString(file.at("text").get<std::string>());
    if (text.isEmpty())
        return 0;
    static const QRegularExpression separator(
        QStringLiteral("\\r\\n|[\\n\\r\\v\\f\\x{001c}-\\x{001e}\\x{0085}\\x{2028}\\x{2029}]"));
    auto parts = text.split(separator);
    return size_t(parts.size() - (parts.back().isEmpty() ? 1 : 0));
}
std::map<uint64_t, uint32_t> boundaries(const std::map<std::string, Bytes> &parts) {
    const auto found = parts.contains("SHEX") ? parts.find("SHEX") : parts.find("SHDR");
    if (found == parts.end())
        throw std::runtime_error("Missing original shader instruction chunk");
    auto program = readDxbcProgram(found->second);
    std::map<uint64_t, uint32_t> result;
    uint64_t offset = 8;
    for (const auto &op : program.instructions) {
        result[offset] = op.at(0) & 2047;
        offset += op.size() * 4;
    }
    return result;
}
std::string disassembly(Bytes raw) { return systemDisassembly(raw); }
QStringList assemblyLines(const std::string &text) {
    return QString::fromStdString(text).split(QRegularExpression("\\r?\\n"));
}
Json sdbgLines(Bytes raw, const std::map<std::string, Bytes> &parts, const Json &source,
               const std::optional<std::string> &assembly) {
    Json result{{"status", "unavailable"},     {"format", "SDBG SDK"},       {"shader_sha256", sha256(raw)},
                {"files", source.at("files")}, {"locations", Json::array()}, {"issues", source.at("issues")}};
    try {
        if (source.at("status") != "embedded_source_available")
            return result;
        const auto data = parts.at("SDBG");
        const auto h = Reader(data).array<uint32_t, 21>();
        const auto base = sub(data, 84, data.size() - 84);
        const auto bounds = boundaries(parts);
        std::map<uint64_t, uint64_t> numbered;
        const QRegularExpression instruction(QStringLiteral("^\\s*(\\d+)\\s+0x([0-9a-fA-F]+):"));
        for (const auto &text : assemblyLines(assembly ? *assembly : disassembly(raw))) {
            const auto match = instruction.match(text);
            if (!match.hasMatch())
                continue;
            const auto index = match.captured(1).toULongLong(),
                       address = match.captured(2).toULongLong(nullptr, 16);
            if (!bounds.contains(address) || !numbered.emplace(index, address).second)
                throw std::runtime_error("Invalid SDBG compiler instruction identity");
        }
        std::vector<uint64_t> addresses;
        size_t index = 0;
        for (const auto &[number, address] : numbered) {
            if (number != index++)
                throw std::runtime_error("Noncontiguous compiler instruction IDs");
            const auto opcode = bounds.at(address);
            if (opcode != 114 && opcode != 115 && opcode != 116)
                addresses.push_back(address);
        }
        std::set<uint32_t> seen;
        Json locations = Json::array();
        for (uint32_t i = 0; i < h[7]; ++i) {
            const auto row = sub(base, uint64_t(h[8]) + uint64_t(i) * 396, 396);
            const auto id = at(row, 0), opcode = at(row, 4), token = at(row, 93 * 4);
            if (!seen.insert(id).second || id >= addresses.size() || bounds.at(addresses[id]) != opcode)
                throw std::runtime_error("SDBG instruction ID/opcode does not match original DXBC");
            if (token == UINT32_MAX)
                continue;
            if (token >= h[13])
                throw std::runtime_error("SDBG instruction token outside table");
            const auto t = sub(base, uint64_t(h[14]) + uint64_t(token) * 20, 20);
            const auto file = at(t, 0), line = at(t, 4);
            if (file == UINT32_MAX)
                continue;
            if (file >= source.at("files").size())
                throw std::runtime_error("SDBG source file outside table");
            const auto &f = source.at("files").at(file);
            if (f.at("text_valid") == true && line >= 1 && line <= lines(f))
                locations.push_back({{"byte_offset", addresses[id]},
                                     {"file", file},
                                     {"line_start", line},
                                     {"line_end", line},
                                     {"statement", true}});
        }
        if (seen.size() != addresses.size())
            throw std::runtime_error("Incomplete SDBG instruction table");
        std::set<std::string> names;
        Json verified = Json::array();
        for (size_t i = 0; i < source.at("files").size(); ++i) {
            const auto &f = source.at("files").at(i);
            if (!names.insert(sourcePathKey(f.at("name"))).second)
                throw std::runtime_error("Ambiguous SDBG source paths");
            if (f.at("text_valid") == true)
                verified.push_back(i);
        }
        result.update({{"status", locations.empty() ? "no_mapped_embedded_lines" : "available"},
                       {"locations", locations},
                       {"verified_source_files", verified},
                       {"navigation_backend", "sdk_sdbg_tokens_with_system_compiler_instruction_offsets"}});
    } catch (const std::exception &e) {
        result["status"] = "invalid_or_unsupported_lines";
        result["locations"] = Json::array();
        result["issues"].push_back(e.what());
    }
    return result;
}
} // namespace

namespace shader_debug {
namespace {
std::string fullCaseFold(const QString &text) {
    std::u32string result;
    for (const auto cp : text.toUcs4()) {
        const auto found =
            std::lower_bound(std::begin(caseFoldEntries), std::end(caseFoldEntries), cp,
                             [](const auto &entry, char32_t value) { return entry.codepoint < value; });
        if (found != std::end(caseFoldEntries) && found->codepoint == cp)
            result.append(caseFoldData + found->offset, found->size);
        else
            result.push_back(cp);
    }
    return QString::fromUcs4(result.data(), qsizetype(result.size())).toStdString();
}
} // namespace
std::string sourcePathKey(const std::string &name) {
    auto path = QString::fromStdString(name);
    path.replace('/', '\\');
    QString prefix;
    // Preserve the drive/UNC root before reducing relative components.
    if (path.startsWith("\\\\")) {
        const auto start = path.startsWith("\\\\?\\UNC\\", Qt::CaseInsensitive) ? 8 : 2;
        const auto server = path.indexOf('\\', start);
        const auto share = server < 0 ? -1 : path.indexOf('\\', server + 1);
        if (share < 0)
            return fullCaseFold(path);
        prefix = path.left(share);
        path = path.mid(share);
    } else if (path.size() >= 2 && path[1] == ':') {
        prefix = path.left(2);
        path = path.mid(2);
    }
    const bool rooted = path.startsWith('\\');
    QStringList components;
    for (const auto &part : path.split('\\', Qt::SkipEmptyParts)) {
        if (part == ".")
            continue;
        if (part == "..") {
            if (!components.empty() && components.back() != "..")
                components.removeLast();
            else if (!rooted)
                components.push_back(part);
        } else
            components.push_back(part);
    }
    auto result = prefix + (rooted ? "\\" : "") + components.join('\\');
    if (result.isEmpty())
        result = ".";
    return fullCaseFold(result);
}
Json inlineeSources(const std::vector<std::vector<uint8_t>> &tables,
                    const std::map<uint32_t, Json> &checksums) {
    Json result = Json::array();
    std::map<uint32_t, size_t> indices;
    for (const auto &data : tables) {
        Reader r(data);
        if (r.remaining() < 4)
            throw std::runtime_error("Truncated inlinee source table");
        const auto signature = r.read<uint32_t>();
        if (signature > 1)
            throw std::runtime_error("Unsupported inlinee source signature");
        while (r.remaining()) {
            const auto id = r.read<uint32_t>(), file = r.read<uint32_t>(), line = r.read<uint32_t>();
            Json extras = Json::array();
            if (signature == 1) {
                const auto count = r.read<uint32_t>();
                if (count > r.remaining() / 4)
                    throw std::runtime_error("Inlinee extra file bounds");
                for (uint32_t i = 0; i < count; ++i)
                    extras.push_back(r.read<uint32_t>());
            }
            if (!checksums.contains(file) || std::any_of(extras.begin(), extras.end(), [&](const auto &f) {
                    return !checksums.contains(f.template get<uint32_t>());
                }))
                throw std::runtime_error("Unknown inlinee file checksum");
            Json row{{"inlinee", id}, {"file_id", file}, {"line", line}, {"extra_files", extras}};
            if (indices.contains(id)) {
                if (result.at(indices.at(id)) != row)
                    throw std::runtime_error("Conflicting inlinee source records");
            } else {
                indices[id] = result.size();
                result.push_back(row);
            }
        }
    }
    return result;
}
} // namespace shader_debug

Json shaderSourceLines(Bytes raw, std::optional<std::string> assembly) {
    using namespace shader_debug;
    const auto parts = chunks(raw);
    const auto source = embedded(parts, true);
    Json result{{"status", "unavailable"},    {"files", source.at("files")},
                {"locations", Json::array()}, {"issues", source.at("issues")},
                {"format", "SPDB C13"},       {"shader_sha256", sha256(raw)}};
    if (!parts.contains("SPDB"))
        return parts.contains("SDBG") ? sdbgLines(raw, parts, source, assembly) : result;
    try {
        Pdb pdb(parts.at("SPDB"));
        const auto names = pdb.names();
        const auto nameData = pdb.stream(names.at("/names"));
        Reader nameReader(nameData);
        if (nameReader.read<uint32_t>() != 0xeffeeffe || nameReader.read<uint32_t>() != 1)
            throw std::runtime_error("Unsupported SPDB string table");
        const auto strings = nameReader.take(nameReader.read<uint32_t>());
        std::map<std::string, size_t> files;
        for (size_t i = 0; i < source.at("files").size(); ++i) {
            const auto &file = source.at("files").at(i);
            const auto key = sourcePathKey(file.at("name"));
            if (files.contains(key))
                throw std::runtime_error("Ambiguous embedded source path");
            if (file.at("text_valid") == true)
                files[key] = i;
        }
        const auto bounds = boundaries(parts);
        const auto code = parts.at(parts.contains("SHEX") ? "SHEX" : "SHDR");
        const auto dbi = pdb.stream(3);
        if (dbi.size() < 64 || at(dbi, 0) != UINT32_MAX || at(dbi, 4) != 19990903)
            throw std::runtime_error("Unsupported SPDB DBI header");
        const auto modules = sub(dbi, 64, at(dbi, 24));
        size_t pos = 0;
        Json locations = Json::array(), scopeModules = Json::array();
        while (pos < modules.size()) {
            const auto row = sub(modules, pos, 64);
            const auto stream = at<uint16_t>(row, 34);
            const auto symbols = at(row, 36), c11 = at(row, 40), c13 = at(row, 44);
            auto moduleEnd = stringAt(modules, pos + 64).second;
            moduleEnd = stringAt(modules, moduleEnd).second;
            pos = (moduleEnd + 3) & ~size_t(3);
            if (pos > modules.size())
                throw std::runtime_error("SPDB module alignment bounds");
            if (stream == 0xffff)
                continue;
            const auto storage = pdb.stream(stream);
            const Bytes data(storage);
            if (symbols < 4 || uint64_t(symbols) + c11 + c13 > data.size())
                throw std::runtime_error("SPDB C13 module bounds");
            if (c11)
                throw std::runtime_error("Legacy C11 line tables are not supported");
            std::map<uint32_t, std::vector<std::vector<uint8_t>>> sections;
            size_t cur = symbols, stop = cur + c13;
            while (cur < stop) {
                if (stop - cur < 8)
                    throw std::runtime_error("Truncated C13 subsection");
                const auto kind = at(data, cur), size = at(data, cur + 4);
                const auto start = cur + 8;
                cur = (start + size + 3) & ~size_t(3);
                if (cur > stop)
                    throw std::runtime_error("C13 subsection bounds");
                if (!(kind & 0x80000000)) {
                    auto bytes = sub(data, start, size);
                    sections[kind].emplace_back(bytes.begin(), bytes.end());
                }
            }
            if (sections[0xf2].empty())
                continue;
            if (sections[0xf4].size() != 1)
                throw std::runtime_error("Missing or ambiguous C13 checksum table");
            std::map<uint32_t, Json> checksums;
            const Bytes checksumTable(sections[0xf4][0]);
            cur = 0;
            while (cur < checksumTable.size()) {
                if (checksumTable.size() - cur < 6)
                    throw std::runtime_error("Truncated source checksum");
                const auto name = at(checksumTable, cur);
                const auto size = checksumTable[cur + 4], kind = checksumTable[cur + 5];
                const auto end = cur + 6 + size;
                if (end > checksumTable.size())
                    throw std::runtime_error("Source checksum bounds");
                const auto filename = utf8(stringAt(strings, name).first);
                const auto found = files.find(sourcePathKey(filename));
                Json index = found == files.end() ? Json(nullptr) : Json(found->second);
                if (!index.is_null()) {
                    const unsigned sizes[]{0, 16, 20, 32};
                    if (kind > 3 || size != sizes[kind])
                        throw std::runtime_error("Unsupported source checksum");
                    if (kind) {
                        const QCryptographicHash::Algorithm algorithms[]{
                            QCryptographicHash::Md5, QCryptographicHash::Sha1, QCryptographicHash::Sha256};
                        const auto bytes = QByteArray::fromHex(
                            QByteArray::fromStdString(source.at("files").at(found->second).at("raw_hex")));
                        const auto digest = QCryptographicHash::hash(bytes, algorithms[kind - 1]);
                        if (std::memcmp(digest.data(), checksumTable.data() + cur + 6, size))
                            throw std::runtime_error("Embedded source checksum mismatch");
                    }
                }
                checksums[uint32_t(cur)] = index;
                cur = (end + 3) & ~size_t(3);
                if (cur > checksumTable.size())
                    throw std::runtime_error("Source checksum alignment bounds");
            }
            Json checks = Json::object();
            for (const auto &[offset, file] : checksums)
                checks[std::to_string(offset)] = file;
            Json module{{"stream", stream},
                        {"checksums", checks},
                        {"base_locations", Json::array()},
                        {"inlinees", Json::array()},
                        {"inlinee_issues", Json::array()}};
            try {
                module["inlinees"] = inlineeSources(sections[0xf6], checksums);
            } catch (const std::exception &e) {
                module["inlinee_issues"].push_back(e.what());
            }
            for (const auto &lineTable : sections[0xf2]) {
                const Bytes table(lineTable);
                if (table.size() < 12)
                    throw std::runtime_error("Truncated C13 lines header");
                const auto base = at(table, 0), length = at(table, 8);
                const auto segment = at<uint16_t>(table, 4), flags = at<uint16_t>(table, 6);
                cur = 12;
                if (segment != 1 || (flags & ~1) || uint64_t(base) + length > code.size())
                    throw std::runtime_error("Unsupported C13 code contribution");
                while (cur < table.size()) {
                    if (table.size() - cur < 12)
                        throw std::runtime_error("Truncated C13 file block");
                    const auto id = at(table, cur), count = at(table, cur + 4), size = at(table, cur + 8);
                    const auto end = cur + size;
                    if (size != 12 + uint64_t(count) * ((flags & 1) ? 12 : 8) || end > table.size() ||
                        !checksums.contains(id))
                        throw std::runtime_error("C13 file block bounds");
                    const auto file = checksums.at(id);
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto relative = at(table, cur + 12 + i * 8), bits = at(table, cur + 16 + i * 8);
                        const uint64_t address = uint64_t(base) + relative;
                        if (relative >= length || !bounds.contains(address))
                            throw std::runtime_error("C13 line is not an original instruction boundary");
                        const auto first = bits & 0xffffff, last = first + ((bits >> 24) & 127);
                        const size_t columns = cur + 12 + uint64_t(count) * 8 + i * 4;
                        const bool valid = !file.is_null() && first >= 1 &&
                                           last <= lines(source.at("files").at(file.get<size_t>()));
                        Json location{{"byte_offset", address},
                                      {"file", valid ? file : Json(nullptr)},
                                      {"line_start", first},
                                      {"line_end", last},
                                      {"column_start", (flags & 1) ? at<uint16_t>(table, columns) : 0},
                                      {"column_end", (flags & 1) ? at<uint16_t>(table, columns + 2) : 0},
                                      {"statement", bool(bits >> 31)}};
                        module["base_locations"].push_back(location);
                        if (valid)
                            locations.push_back(location);
                    }
                    cur = end;
                }
            }
            scopeModules.push_back(module);
        }
        result["c13_locations"] = locations;
        result["scope_modules"] = scopeModules;
        Json resolved = Json::array();
        std::optional<size_t> file;
        std::optional<uint64_t> line;
        const QRegularExpression directive(QStringLiteral("^#line (\\d+)(?: \"(.*)\")?$"));
        const QRegularExpression instruction(QStringLiteral("^\\s*\\d+\\s+0x([0-9a-fA-F]+):"));
        for (const auto &text : assemblyLines(assembly ? *assembly : disassembly(raw))) {
            const auto d = directive.match(text);
            if (d.hasMatch()) {
                line = d.captured(1).toULongLong();
                if (!d.captured(2).isNull()) {
                    const auto found = files.find(sourcePathKey(d.captured(2).toStdString()));
                    file = found == files.end() ? std::nullopt : std::optional<size_t>(found->second);
                }
            }
            const auto inst = instruction.match(text);
            if (inst.hasMatch() && file && line) {
                const auto address = inst.captured(1).toULongLong(nullptr, 16);
                if (!bounds.contains(address))
                    throw std::runtime_error(
                        "Compiler source directive is not an original instruction boundary");
                if (*line >= 1 && *line <= lines(source.at("files").at(*file)))
                    resolved.push_back({{"byte_offset", address},
                                        {"file", *file},
                                        {"line_start", *line},
                                        {"line_end", *line},
                                        {"statement", true}});
            }
        }
        result.update({{"status", resolved.empty() ? "no_mapped_embedded_lines" : "available"},
                       {"locations", resolved},
                       {"navigation_backend", "system_d3dcompiler_source_directives"}});
    } catch (const std::exception &e) {
        result["status"] = "invalid_or_unsupported_lines";
        result["locations"] = Json::array();
        result["issues"].push_back(e.what());
    }
    return result;
}
Json shaderLinesByOffset(const Json &report) {
    std::map<uint64_t, std::vector<Json>> groups;
    for (const auto &row : report.at("locations"))
        groups[row.at("byte_offset").get<uint64_t>()].push_back(row);
    Json result = Json::object();
    for (const auto &[offset, rows] : groups) {
        const bool expressions =
            std::any_of(rows.begin(), rows.end(), [](const auto &r) { return r.at("statement") == false; });
        std::set<std::tuple<Json, Json, Json>> values;
        for (const auto &row : rows)
            if (!expressions || row.at("statement") == false)
                values.emplace(row.at("file"), row.at("line_start"), row.at("line_end"));
        if (values.size() == 1) {
            const auto &[file, first, last] = *values.begin();
            result[std::to_string(offset)] = {{"file", file}, {"line_start", first}, {"line_end", last}};
        }
    }
    return result;
}
} // namespace flora
