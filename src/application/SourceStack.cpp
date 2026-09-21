#include "SourceStack.h"
#include "DxbcCheckpointModel.h"
#include "ShaderDebugData.h"
#include <QRegularExpression>
#include <tuple>

namespace flora {
namespace {
using Json = nlohmann::json;
using Locations = std::set<std::tuple<int64_t, int64_t, int64_t>>;
std::map<std::string, Json> indexed(const Json &rows) {
    std::map<std::string, Json> result;
    for (const auto &row : rows)
        result[row.at("id").get<std::string>()] = row;
    return result;
}
std::vector<uint8_t> annotations(const Json &scope) {
    const auto text = QString::fromStdString(scope.at("annotations").get<std::string>());
    static const QRegularExpression valid(
        QStringLiteral("^[\\t \\r\\n\\f\\v]*(?:[0-9a-fA-F]{2}[\\t \\r\\n\\f\\v]*)*$"));
    if (!valid.match(text).hasMatch())
        throw std::runtime_error("Invalid inline annotation hex");
    const auto raw = QByteArray::fromHex(text.toLatin1());
    return {raw.begin(), raw.end()};
}
size_t lineCount(const Json &file) {
    const auto text = QString::fromStdString(file.at("text").get<std::string>());
    if (text.isEmpty())
        return 0;
    static const QRegularExpression separator(
        QStringLiteral("\\r\\n|[\\n\\r\\v\\f\\x{001c}-\\x{001e}\\x{0085}\\x{2028}\\x{2029}]"));
    const auto lines = text.split(separator);
    return size_t(lines.size() - (lines.back().isEmpty() ? 1 : 0));
}
Json locationRow(const std::tuple<int64_t, int64_t, int64_t> &location) {
    const auto &[file, first, last] = location;
    return {{"file", file}, {"line_start", first}, {"line_end", last}};
}
void attachLocations(Json &model, const Json &symbols, const Json &source,
                     const std::set<uint64_t> &boundaries) {
    model["location_issues"] = Json::array();
    if (source.value("shader_sha256", Json(nullptr)) != model.at("shader_sha256") ||
        source.value("status", "") != "available") {
        model["location_issues"].push_back("Matching source line model is unavailable");
        return;
    }
    const auto scopes = indexed(symbols.at("scopes")), frames = indexed(model.at("frames"));
    std::map<uint32_t, Json> modules;
    for (const auto &module : source.value("scope_modules", Json::array()))
        modules[module.at("stream").get<uint32_t>()] = module;
    std::vector<size_t> lengths;
    for (const auto &file : source.at("files"))
        lengths.push_back(lineCount(file));
    auto valid = [&](const Json &file, int64_t first, int64_t last) {
        return file.is_number_integer() && file.get<int64_t>() >= 0 &&
               file.get<uint64_t>() < lengths.size() && first >= 1 && first <= last &&
               uint64_t(last) <= lengths[file.get<size_t>()];
    };
    for (auto &frame : model["frames"]) {
        frame["locations"] = Json::array();
        const auto id = frame.at("id").get<std::string>();
        const auto &scope = scopes.at(id);
        const auto stream = std::stoul(id.substr(0, id.find(':')));
        if (stream > UINT32_MAX || !modules.contains(uint32_t(stream)))
            continue;
        const auto &module = modules.at(uint32_t(stream));
        if (frame.at("kind") == "function") {
            std::map<uint64_t, Json> grouped;
            const auto start = scope.at("code_start").get<uint64_t>(),
                       end = scope.at("code_end").get<uint64_t>();
            for (const auto &row : module.at("base_locations")) {
                const auto offset = row.at("byte_offset").get<uint64_t>();
                if (start <= offset && offset < end) {
                    if (!grouped.contains(offset))
                        grouped[offset] = Json::array();
                    grouped[offset].push_back(row);
                }
            }
            for (auto it = grouped.begin(); it != grouped.end(); ++it) {
                const auto &rows = it->second;
                const bool expressions = std::any_of(
                    rows.begin(), rows.end(), [](const auto &r) { return r.at("statement") == false; });
                std::set<Json> unique;
                for (const auto &row : rows)
                    if (!expressions || row.at("statement") == false) {
                        unique.insert(
                            Json::array({row.at("file"), row.at("line_start"), row.at("line_end")}));
                    }
                if (unique.size() != 1)
                    continue;
                const auto &location = *unique.begin();
                const auto &file = location.at(0);
                const auto first = location.at(1).get<int64_t>(), last = location.at(2).get<int64_t>();
                if (!valid(file, first, last))
                    continue;
                Json row{{"file", file}, {"line_start", first}, {"line_end", last}};
                row["start"] = it->first;
                const auto next = std::next(it);
                row["end"] = next == grouped.end() ? end : next->first;
                frame["locations"].push_back(std::move(row));
            }
        } else {
            std::map<uint32_t, Json> inlinees;
            for (const auto &entry : module.at("inlinees"))
                inlinees[entry.at("inlinee").get<uint32_t>()] = entry;
            const auto ident = scope.at("inlinee").get<uint32_t>();
            if (!module.at("inlinee_issues").empty() || !inlinees.contains(ident)) {
                if (module.at("inlinee_issues").empty())
                    model["location_issues"].push_back("Missing inlinee source definition");
                else
                    for (const auto &issue : module.at("inlinee_issues"))
                        model["location_issues"].push_back(issue);
                continue;
            }
            const auto &definition = inlinees.at(ident);
            auto root = frame;
            std::set<std::string> seen;
            while (!root.at("parent").is_null()) {
                const auto parent = root.at("parent").get<std::string>();
                if (!seen.insert(parent).second)
                    throw std::runtime_error("Recursive source frame parent");
                root = frames.at(parent);
            }
            const auto ranges = codeview::statementRanges(
                annotations(scope), scopes.at(root.at("id").get<std::string>()).at("code_start"), boundaries,
                true);
            for (const auto &row : ranges) {
                const auto fileId =
                    row.at("file_id").is_null() ? definition.at("file_id") : row.at("file_id");
                const auto file =
                    module.at("checksums").value(std::to_string(fileId.get<uint32_t>()), Json(nullptr));
                const auto first = definition.at("line").get<int64_t>() + row.at("line_delta").get<int64_t>();
                const auto last = first + row.at("line_end_delta").get<int64_t>();
                if (valid(file, first, last))
                    frame["locations"].push_back({{"start", row.at("start")},
                                                  {"end", row.at("end")},
                                                  {"file", file},
                                                  {"line_start", first},
                                                  {"line_end", last}});
            }
        }
    }
}
std::optional<std::string> owner(const std::map<std::string, Json> &scopes, const std::string &id) {
    Json current = id;
    std::set<std::string> seen;
    while (current.is_string()) {
        const auto key = current.get<std::string>();
        if (!scopes.contains(key) || scopes.at(key).at("kind") != "block" || !seen.insert(key).second)
            return key;
        current = scopes.at(key).at("parent");
    }
    return {};
}
void attachHull(const checkpoint::Rows &ops, Json &symbols, Json &model) {
    std::vector<uint64_t> offsets;
    uint64_t offset = 8;
    for (const auto &row : ops) {
        offsets.push_back(offset);
        offset += row.size() * 4;
    }
    offsets.push_back(offset);
    const auto scopes = indexed(symbols.at("scopes"));
    Json records = Json::array();
    for (const auto &phase : checkpoint::hullPhases(ops)) {
        const auto start = offsets.at(phase.at("split").get<size_t>()),
                   end = offsets.at(phase.at("end").get<size_t>());
        std::set<std::string> allowed;
        for (const auto &frame : model.at("frames")) {
            if (phase.at("kind") != "control_points" && frame.at("parent").is_null())
                continue;
            for (const auto &r : frame.at("ranges"))
                if (r.at("start").get<uint64_t>() < end && start < r.at("end").get<uint64_t>())
                    allowed.insert(frame.at("id").get<std::string>());
        }
        Json owned = Json::array();
        for (const auto &scope : symbols.at("scopes")) {
            const auto current = owner(scopes, scope.at("id"));
            if (current && allowed.contains(*current))
                owned.push_back(scope.at("id"));
        }
        records.push_back({{"id", phase.at("id")},
                           {"kind", phase.at("kind")},
                           {"start", start},
                           {"end", end},
                           {"frame_ids", allowed},
                           {"scope_ids", owned}});
    }
    model["hs_phases"] = records;
    symbols["hs_phases"] = records;
    model["scope_semantics"] =
        "phase_local_inline_scopes; control-point entry wrapper excluded from fork/join";
}
} // namespace
namespace codeview {
std::pair<uint32_t, size_t> compressed(Bytes data, size_t offset) {
    if (offset >= data.size())
        throw std::runtime_error("Truncated inline annotation");
    const auto first = data[offset];
    const size_t size = first < 0x80 ? 1 : first < 0xc0 ? 2 : first < 0xe0 ? 4 : 0;
    if (!size || size > data.size() - offset)
        throw std::runtime_error("Invalid compressed inline annotation");
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i)
        value = (value << 8) | data[offset + i];
    if (size > 1)
        value &= size == 2 ? 0x3fffu : 0x1fffffffu;
    return {value, offset + size};
}
Json statementRanges(Bytes data, uint64_t base, const std::set<uint64_t> &boundaries, bool details) {
    size_t pos = 0;
    uint64_t cursor = 0;
    std::optional<uint64_t> pending;
    uint32_t kind = 1;
    int64_t line = 0;
    uint32_t lineEnd = 1;
    Json result = Json::array(), file = nullptr, pendingSource = Json::object();
    auto signedValue = [](uint32_t value) { return value & 1 ? -int64_t(value >> 1) : int64_t(value >> 1); };
    auto finish = [&](uint64_t end) {
        if (!pending)
            return;
        if (end <= *pending || base > UINT64_MAX - end || !boundaries.contains(base + *pending) ||
            !boundaries.contains(base + end))
            throw std::runtime_error("Inline statement instruction bounds");
        Json row{{"start", base + *pending}, {"end", base + end}};
        if (details)
            row.update(pendingSource);
        result.push_back(std::move(row));
        pending.reset();
    };
    auto read = [&]() {
        auto [value, next] = compressed(data, pos);
        pos = next;
        return value;
    };
    while (pos < data.size()) {
        const auto opcode = read();
        if (!opcode) {
            if (std::any_of(data.begin() + pos, data.end(), [](auto byte) { return byte != 0; }))
                throw std::runtime_error("Nonzero inline annotation padding");
            break;
        }
        if (opcode > 13)
            throw std::runtime_error("Unsupported inline annotation opcode");
        const auto value = read(), extra = opcode == 12 ? read() : 0;
        if (opcode == 2 && value)
            throw std::runtime_error("Separated inline code chunks are unsupported");
        if (opcode == 5)
            file = value;
        else if (opcode == 6)
            line += signedValue(value);
        else if (opcode == 7)
            lineEnd = value;
        if (opcode == 8) {
            if (value > 1)
                throw std::runtime_error("Invalid inline range kind");
            if (pending)
                throw std::runtime_error("Unterminated inline statement range");
            kind = value;
        } else if (opcode == 1) {
            if (pending)
                throw std::runtime_error("Unterminated inline range before offset reset");
            cursor = value;
        } else if (opcode == 3 || opcode == 11 || opcode == 12) {
            cursor += opcode == 12 ? extra : opcode == 11 ? value & 15 : value;
            if (opcode == 11)
                line += signedValue(value >> 4);
            if (kind == 1) {
                finish(cursor);
                pending = cursor;
                pendingSource = {{"file_id", file}, {"line_delta", line}, {"line_end_delta", lineEnd}};
                if (opcode == 12)
                    finish(cursor + value);
            }
        } else if (opcode == 4) {
            if (kind == 1) {
                if (!pending)
                    throw std::runtime_error("Inline length without a start");
                finish(cursor + value);
            }
            cursor += value;
        }
        if (cursor > UINT32_MAX)
            throw std::runtime_error("Inline code offset overflow");
    }
    if (pending)
        throw std::runtime_error("Inline statement missing final length");
    return result;
}
} // namespace codeview
Json sourceStack(Bytes raw, Json &symbols, const Json &source) {
    Json result{{"status", "unavailable"},
                {"shader_sha256", sha256(raw)},
                {"frames", Json::array()},
                {"issues", Json::array()}};
    if (symbols.value("format", "") == "SDBG assignments") {
        result["issues"].push_back("SDBG scope snapshots are not a reconstructed function stack");
        return result;
    }
    const auto status = symbols.value("status", "");
    if (status != "available" && status != "no_local_symbols")
        return result;
    try {
        if (symbols.at("shader_sha256") != result.at("shader_sha256"))
            throw std::runtime_error("Source scope shader mismatch");
        const auto parts = shader_debug::chunks(raw);
        const auto code = parts.at(parts.contains("SHEX") ? "SHEX" : "SHDR");
        const auto program = readDxbcProgram(code);
        std::set<uint64_t> boundaries{code.size()};
        uint64_t offset = 8;
        for (const auto &op : program.instructions) {
            boundaries.insert(offset);
            offset += op.size() * 4;
        }
        const auto scopes = indexed(symbols.at("scopes"));
        for (const auto &scope : symbols.at("scopes")) {
            if (scope.at("kind") == "block")
                continue;
            Json parent = scope.at("parent");
            std::vector<std::string> ancestors;
            while (!parent.is_null()) {
                const auto id = parent.get<std::string>();
                if (ancestors.size() >= 64 ||
                    std::find(ancestors.begin(), ancestors.end(), id) != ancestors.end())
                    throw std::runtime_error("Recursive source frame parent");
                ancestors.push_back(id);
                parent = scopes.at(id).at("parent");
            }
            std::vector<Json> functions;
            Json frameParent = nullptr;
            for (const auto &id : ancestors) {
                const auto &ancestor = scopes.at(id);
                if (ancestor.at("kind") == "function")
                    functions.push_back(ancestor);
                if (frameParent.is_null() && ancestor.at("kind") != "block")
                    frameParent = id;
            }
            Json frame{{"id", scope.at("id")},
                       {"name", scope.at("name")},
                       {"kind", scope.at("kind")},
                       {"parent", frameParent}};
            if (scope.at("kind") == "function") {
                if (!ancestors.empty())
                    throw std::runtime_error("Nested non-inline source function");
                frame["ranges"] =
                    Json::array({{{"start", scope.at("code_start")}, {"end", scope.at("code_end")}}});
            } else {
                if (functions.size() != 1)
                    throw std::runtime_error("Inline scope without one original function");
                const auto &function = functions.at(0);
                frame["ranges"] =
                    codeview::statementRanges(annotations(scope), function.at("code_start"), boundaries);
                for (const auto &r : frame.at("ranges"))
                    if (r.at("start") < function.at("code_start") || r.at("end") > function.at("code_end"))
                        throw std::runtime_error("Inline statements outside original function");
            }
            result["frames"].push_back(std::move(frame));
        }
        result["status"] = result.at("frames").empty() ? "no_function_symbols" : "available";
        attachLocations(result, symbols, source, boundaries);
        if (Reader(code).read<uint32_t>() >> 16 == 3)
            attachHull(program.instructions, symbols, result);
    } catch (const std::exception &e) {
        result.update({{"status", "invalid_or_unsupported_ranges"},
                       {"frames", Json::array()},
                       {"issues", Json::array({e.what()})}});
    }
    return result;
}
Json sourceStackAt(const Json &model, uint64_t offset, uint32_t depth) {
    auto absent = [](const char *status) { return Json{{"status", status}, {"frames", Json::array()}}; };
    if (model.value("status", "") != "available")
        return absent("unavailable");
    if (depth)
        return absent("unmapped_subroutine");
    const auto frames = indexed(model.at("frames"));
    std::set<std::string> active;
    for (const auto &[id, frame] : frames)
        for (const auto &r : frame.at("ranges"))
            if (r.at("start").get<uint64_t>() <= offset && offset < r.at("end").get<uint64_t>())
                active.insert(id);
    if (active.empty())
        return absent("unmapped");
    std::vector<std::vector<std::string>> paths;
    for (const auto &id : active) {
        std::vector<std::string> path;
        std::set<std::string> seen;
        Json current = id;
        while (!current.is_null()) {
            const auto key = current.get<std::string>();
            if (!active.contains(key) || !seen.insert(key).second)
                return absent("ambiguous");
            path.push_back(key);
            current = frames.at(key).at("parent");
        }
        if (seen == active) {
            std::reverse(path.begin(), path.end());
            paths.push_back(std::move(path));
        }
    }
    if (paths.size() != 1)
        return absent("ambiguous");
    Json selected = Json::array();
    for (const auto &id : paths.at(0))
        selected.push_back(
            {{"id", id}, {"name", frames.at(id).at("name")}, {"kind", frames.at(id).at("kind")}});
    if (!model.contains("hs_phases"))
        return {{"status", "available"}, {"frames", selected}};
    Json phases = Json::array();
    for (const auto &p : model.at("hs_phases"))
        if (p.at("start").get<uint64_t>() <= offset && offset < p.at("end").get<uint64_t>())
            phases.push_back(p);
    if (phases.size() != 1)
        return absent("unmapped_hs_phase");
    const auto &phase = phases.at(0);
    Json local = Json::array();
    for (const auto &frame : selected)
        if (std::find(phase.at("frame_ids").begin(), phase.at("frame_ids").end(), frame.at("id")) !=
            phase.at("frame_ids").end())
            local.push_back(frame);
    if (local.empty()) {
        auto result = absent("unmapped_hs_phase_scope");
        result["hs_phase"] = phase.at("id");
        return result;
    }
    return {{"status", "available"},
            {"frames", local},
            {"hs_phase", phase.at("id")},
            {"phase_kind", phase.at("kind")},
            {"scope_semantics", model.at("scope_semantics")}};
}
Json sourceFrameLocation(const Json &model, const std::string &id, uint64_t offset) {
    for (const auto &frame : model.value("frames", Json::array()))
        if (frame.at("id") == id) {
            Locations matches;
            for (const auto &r : frame.value("locations", Json::array()))
                if (r.at("start").get<uint64_t>() <= offset && offset < r.at("end").get<uint64_t>())
                    matches.emplace(r.at("file").get<int64_t>(), r.at("line_start").get<int64_t>(),
                                    r.at("line_end").get<int64_t>());
            return matches.size() == 1 ? locationRow(*matches.begin()) : Json(nullptr);
        }
    return nullptr;
}
Json sourceFrameLocals(const Json &symbols, const Json &values, const std::string &frameId) {
    const auto scopes = indexed(symbols.value("scopes", Json::array()));
    std::set<std::string> owned;
    for (const auto &[id, scope] : scopes)
        if (owner(scopes, id) == frameId)
            owned.insert(id);
    Json result = Json::array();
    for (const auto &v : values)
        if (v.at("scope_id").is_string() && owned.contains(v.at("scope_id").get<std::string>()))
            result.push_back(v);
    return result;
}
} // namespace flora
