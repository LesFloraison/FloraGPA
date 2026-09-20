#include "OutputLogGeometry.h"
#include <QDir>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace flora {
using Json = nlohmann::json;
bool isOutputLogStage(const std::string &stage) {
    return stage == "vs-writes" || stage == "ds-writes" || stage == "gs-emits";
}
namespace {
const Json &metadata(const PostTransformGeometry &g) {
    return g.report.at(g.report.at("shader_stage") == "gs"   ? "geometry_emissions"
                       : g.report.at("shader_stage") == "ds" ? "domain_writes"
                                                             : "vertex_writes");
}
Json attributes(const Json &signature) {
    Json result = Json::array();
    unsigned stride = 0;
    for (auto field : signature) {
        const auto mask = field.at("mask").get<unsigned>(), type = field.at("component_type").get<unsigned>();
        if (!mask || mask > 15 || type < 1 || type > 3)
            throw std::runtime_error("Unsupported output record signature");
        Json components = Json::array(), registers = Json::array();
        unsigned first = 0;
        while (!(mask & (1u << first)))
            ++first;
        for (unsigned c = 0; c < 4; ++c)
            if (mask & (1u << c)) {
                components.push_back(c - first);
                registers.push_back(c);
            }
        field.update(Json{{"components", components},
                          {"register_components", registers},
                          {"offset", stride},
                          {"component_count", components.size()}});
        stride += unsigned(components.size()) * 4;
        result.push_back(std::move(field));
    }
    return result;
}
uint32_t word(Bytes bytes, size_t at) {
    if (at > bytes.size())
        throw std::runtime_error("Output record field bounds");
    return Reader(bytes.subspan(at)).read<uint32_t>();
}
Json number(Bytes bytes, size_t at, unsigned type) {
    if (at > bytes.size())
        throw std::runtime_error("Output record value bounds");
    Reader reader(bytes.subspan(at));
    if (type == 1)
        return reader.read<uint32_t>();
    if (type == 2)
        return reader.read<int32_t>();
    if (type != 3)
        throw std::runtime_error("Unsupported output record type");
    const auto f = reader.read<float>();
    return std::isfinite(f) ? Json(f) : Json(std::isnan(f) ? "nan" : f < 0 ? "-inf" : "inf");
}
struct Presentation {
    Json ui, primitives;
    std::vector<uint8_t> values, validity;
};
Presentation present(const PostTransformGeometry &g) {
    const auto &m = metadata(g);
    const auto count = m.at("records").get<uint64_t>(), recordStride = m.at("record_stride").get<uint64_t>();
    const auto registers = m.at("registers").get<uint32_t>();
    const bool gs = m.at("shader_stage") == "gs", ds = m.at("shader_stage") == "ds";
    if (!recordStride || count > g.bytes.size() / recordStride || count * recordStride != g.bytes.size() ||
        registers > 32 || recordStride != (gs ? 32 : 16) + registers * 32 ||
        m.at("record_ordinals").size() != count)
        throw std::runtime_error("Output log record storage bounds");
    const auto &fields = g.report.at("attributes");
    const auto stride = g.report.at("stride").get<unsigned>();
    Presentation result;
    result.primitives = Json::array();
    for (uint64_t i = 0; i < count; ++i)
        for (const auto &field : fields) {
            const auto reg = field.at("register").get<unsigned>();
            if (reg >= registers)
                throw std::runtime_error("Output log signature register exceeds allocation");
            for (const auto &component : field.at("register_components")) {
                const auto c = component.get<unsigned>();
                if (c > 3)
                    throw std::runtime_error("Output log signature component bounds");
                const auto at = i * recordStride + (gs ? 32 : 16) + reg * 16 + c * 4;
                result.values.insert(result.values.end(), g.bytes.begin() + at, g.bytes.begin() + at + 4);
                result.validity.insert(result.validity.end(), g.bytes.begin() + at + registers * 16,
                                       g.bytes.begin() + at + registers * 16 + 4);
            }
        }
    if (result.values.size() != count * stride)
        throw std::runtime_error("Packed output log stride mismatch");
    Json columns = gs   ? Json{"record",       "invocation",  "ordinal", "draw_instance",
                             "primitive_id", "gs_instance", "stream",  "operation"}
                   : ds ? Json{"record", "instance", "primitive_id", "domain.u", "domain.v", "domain.w"}
                        : Json{"record", "instance", "vertex_id"};
    for (const auto &field : fields)
        for (const auto &component : field.at("components")) {
            const auto name = field.at("semantic").get<std::string>() +
                              std::to_string(field.at("index").get<unsigned>()) + "." +
                              "xyzw"[component.get<unsigned>()];
            columns.push_back(name);
            columns.push_back(name + ".written");
        }
    Json rows = Json::array();
    std::vector<Json> operations;
    const auto &known = m.at("known_inputs");
    if (gs) {
        const std::array<const char *, 4> names{"", "emit", "cut", "emit_then_cut"};
        for (uint64_t i = 0; i < count; ++i) {
            auto header = Reader(Bytes(g.bytes).subspan(i * recordStride)).array<uint32_t, 6>();
            if (!header[5] || header[5] > 3 || header[4] != m.at("stream") ||
                header[0] >= m.at("invocations").get<uint64_t>())
                throw std::runtime_error("Invalid GS emission record header");
            operations.push_back(
                {{"record", i},
                 {"invocation", header[0]},
                 {"ordinal", header[1]},
                 {"primitive_id", known.contains("primitive") ? Json(header[2]) : Json(nullptr)},
                 {"gs_instance", known.contains("gs_instance") ? Json(header[3]) : Json(nullptr)},
                 {"stream", header[4]},
                 {"operation", names[header[5]]}});
        }
        std::sort(operations.begin(), operations.end(), [](const Json &a, const Json &b) {
            return std::pair{a.at("invocation").get<uint32_t>(), a.at("ordinal").get<uint32_t>()} <
                   std::pair{b.at("invocation").get<uint32_t>(), b.at("ordinal").get<uint32_t>()};
        });
        std::optional<uint32_t> current;
        uint64_t ordinal = 0;
        std::vector<uint64_t> pending;
        const auto topology = m.at("topology").get<std::string>();
        if (topology != "pointlist" && topology != "linestrip" && topology != "trianglestrip")
            throw std::runtime_error("Unknown emission topology");
        for (const auto &row : operations) {
            const auto invocation = row.at("invocation").get<uint32_t>();
            if (current != invocation) {
                current = invocation;
                ordinal = 0;
                pending.clear();
            }
            if (row.at("ordinal") != ordinal++)
                throw std::runtime_error("GS emission has missing or repeated operation ordinals");
            const auto kind = row.at("operation").get<std::string>();
            if (kind != "cut") {
                pending.push_back(row.at("record"));
                const auto n = pending.size();
                const auto needed = topology == "pointlist" ? 1u : topology == "linestrip" ? 2u : 3u;
                if (n >= needed) {
                    std::vector<uint64_t> indices(pending.end() - needed, pending.end());
                    if (needed == 3 && n % 2 == 0)
                        std::swap(indices[1], indices[2]);
                    result.primitives.push_back({{"primitive", result.primitives.size()},
                                                 {"invocation", invocation},
                                                 {"stream", row.at("stream")},
                                                 {"records", indices}});
                }
            }
            if (kind == "cut" || kind == "emit_then_cut")
                pending.clear();
        }
    }
    const auto instances = m.at("instance_count").get<uint32_t>();
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t index = i;
        Json row;
        if (gs) {
            const auto &o = operations[i];
            index = o.at("record");
            row = {index,
                   o.at("invocation"),
                   o.at("ordinal"),
                   instances == 1 ? Json(0) : Json(nullptr),
                   o.at("primitive_id"),
                   o.at("gs_instance"),
                   o.at("stream"),
                   o.at("operation")};
        } else {
            const auto base = i * recordStride;
            const Json instance = known.contains("instance") ? Json(word(g.bytes, base + 4))
                                  : instances == 1           ? Json(0)
                                                             : Json(nullptr);
            row = {m.at("record_ordinals")[i], instance,
                   known.contains(ds ? "primitive" : "vertex") ? Json(word(g.bytes, base)) : Json(nullptr)};
            if (ds) {
                const auto components =
                    known.contains("domain") ? known.at("domain").at("components") : Json::array();
                for (unsigned c = 0; c < 3; ++c)
                    row.push_back(std::find(components.begin(), components.end(), Json(c)) != components.end()
                                      ? number(g.bytes, base + 4 + c * 4, 3)
                                      : Json(nullptr));
            }
        }
        for (const auto &field : fields)
            for (unsigned c = 0; c < field.at("component_count").get<unsigned>(); ++c) {
                const auto at = index * stride + field.at("offset").get<unsigned>() + c * 4;
                const bool written = word(result.validity, at) != 0;
                row.push_back(written ? number(result.values, at, field.at("component_type"))
                                      : Json(nullptr));
                row.push_back(written);
            }
        rows.push_back(std::move(row));
    }
    Json positions = Json::array(), faces = Json::array(), lines = Json::array(), points = Json::array();
    Json reason = g.report.value("obj_unavailable_reason", Json(nullptr));
    if (gs) {
        std::optional<unsigned> position;
        for (const auto &field : fields)
            if (field.at("system_value") == 1 && field.at("component_type") == 3 &&
                field.at("components") == Json{0, 1, 2, 3}) {
                position = field.at("offset");
                break;
            }
        std::map<uint64_t, uint64_t> lookup;
        reason = position ? Json(nullptr) : Json("Selected GS stream has no complete float4 SV_Position");
        if (position)
            for (const auto &op : operations) {
                if (op.at("operation") == "cut")
                    continue;
                const auto index = op.at("record").get<uint64_t>(), at = index * stride + *position;
                auto p = Reader(Bytes(result.values).subspan(at)).array<float, 4>();
                auto validity = Reader(Bytes(result.validity).subspan(at)).array<uint32_t, 4>();
                if (!p[3] || !std::all_of(p.begin(), p.end(), [](float f) { return std::isfinite(f); }) ||
                    !std::all_of(validity.begin(), validity.end(), [](auto v) { return v != 0; })) {
                    reason = "An emitted position is unwritten, nonfinite or has zero W";
                    break;
                }
                lookup[index] = positions.size() + 1;
                positions.push_back({double(p[0]) / p[3], double(p[1]) / p[3], double(p[2]) / p[3]});
            }
        if (!reason.is_null())
            positions.clear();
        else
            for (const auto &primitive : result.primitives) {
                Json indices = Json::array();
                for (const auto &id : primitive.at("records"))
                    indices.push_back(lookup.at(id.get<uint64_t>()));
                if (indices.size() == 1)
                    points.push_back(indices[0]);
                else if (indices.size() == 2)
                    lines.push_back(indices);
                else
                    faces.push_back(indices);
            }
    }
    Json primitiveRows = Json::array();
    for (const auto &p : result.primitives) {
        Json row = {p.at("primitive"), p.at("invocation"), p.at("stream")};
        for (const auto &index : p.at("records"))
            row.push_back(index);
        while (row.size() < 6)
            row.push_back(nullptr);
        primitiveRows.push_back(std::move(row));
    }
    result.ui = {
        {"event", std::to_string(g.report.at("event").at("id").get<Id>())},
        {"vertex_references", count},
        {"record_kind", gs ? "emissions" : "invocations"},
        {"obj_vertices", positions.size()},
        {"obj_faces", faces.size()},
        {"obj_lines", lines.size()},
        {"obj_points", points.size()},
        {"obj_unavailable_reason", reason},
        {"tables", {{"expanded_vertices", {{"columns", columns}, {"rows", rows}}}}},
        {"mesh", {{"positions", positions}, {"faces", faces}, {"lines", lines}, {"points", points}}}};
    if (gs)
        result.ui["tables"]["primitives"] = {
            {"columns", {"primitive", "invocation", "stream", "record0", "record1", "record2"}},
            {"rows", primitiveRows}};
    return result;
}
} // namespace
PostTransformGeometry outputLogGeometry(const Json &event, Json m, std::vector<uint8_t> records) {
    const bool gs = m.at("shader_stage") == "gs", ds = m.at("shader_stage") == "ds";
    auto fields = attributes(m.at("signature"));
    unsigned stride = 0;
    for (const auto &f : fields)
        stride += f.at("component_count").get<unsigned>() * 4;
    Json report{{"event", event},
                {"geometry_stage", gs   ? "geometry_shader_emissions"
                                   : ds ? "domain_output_writes"
                                        : "vertex_output_writes"},
                {"requested_stage", gs   ? "gs-emits"
                                    : ds ? "ds-writes"
                                         : "vs-writes"},
                {"shader_stage", m.at("shader_stage")},
                {"stream", gs ? m.at("stream") : Json(0)},
                {"vertex_references", m.at("records")},
                {"vertices", m.at("records")},
                {"stride", stride},
                {"attributes", fields},
                {"enabled", m.at("enabled")},
                {gs   ? "geometry_emissions"
                 : ds ? "domain_writes"
                      : "vertex_writes",
                 m}};
    if (gs)
        report["limits"] = {
            "Diagnostic invocation IDs and allocation order are not an original GPU timeline.",
            "Only original declared PrimitiveID and GSInstanceID are recorded; draw instance membership is "
            "not inferred.",
            "Emission invalidates all output streams; unwritten values remain blank.",
            "CUT has no vertex attributes; binary row index equals record, CSV groups by invocation/ordinal.",
            "Incomplete strip tails stay visible as emissions but do not create primitives.",
            "Invocation completion implicitly terminates its last strip.",
            "Private execution does not guarantee repeated UAV atomic ordering."};
    else {
        report["obj_unavailable_reason"] =
            ds ? "DS invocation records have no primitive assembly order; unknown original identities remain "
                 "blank"
               : "VS invocation records have no primitive order; unknown original IDs remain blank";
        report["limits"] = {
            "Atomic allocation order is diagnostic, not an original GPU timeline.",
            "Adding logging may change execution or atomic scheduling on other programs/devices.",
            "Written flags identify output writes, not proof that their source value was initialized.",
            "Unexecuted tail or unused adjacency vertices are not fabricated.",
            "Original SO requires known tracked in-frame byte cursors; unknown pre-frame history is "
            "rejected."};
        if (ds)
            report["limits"].push_back("PrimitiveID alone is not a global patch identity across instances; "
                                       "no instance membership is inferred.");
    }
    if (!m.at("instance").is_null())
        report["instance_selection"] = {{"instance", m.at("instance")},
                                        {"instance_count", m.at("instance_count")},
                                        {"strategy", !gs && m.at("instance_count").get<uint32_t>() > 1
                                                         ? "consumed_native_vs_instance_id"
                                                         : "single_original_instance"}};
    if (gs)
        report["limits"].push_back(
            "Entry atomic counts and native Pipeline Statistics are separate observations; some drivers skip "
            "entry effects on early-return GS instances.");
    PostTransformGeometry result{std::move(report), std::move(records), {}};
    auto p = present(result);
    for (auto key : {"obj_vertices", "obj_faces", "obj_lines", "obj_points", "obj_unavailable_reason"})
        result.report[key] = p.ui.at(key);
    if (gs) {
        result.report["records"] = m.at("records");
        result.report["vertices"] = std::count_if(p.ui["tables"]["expanded_vertices"]["rows"].begin(),
                                                  p.ui["tables"]["expanded_vertices"]["rows"].end(),
                                                  [](const Json &row) { return row[7] != "cut"; });
        result.report["primitives"] = p.primitives.size();
    }
    return result;
}
Json outputLogTables(const PostTransformGeometry &geometry) { return present(geometry).ui; }
void exportOutputLog(const PostTransformGeometry &geometry, const std::filesystem::path &directory) {
    const auto root = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create output log export directory");
    auto save = [&](const QString &name, const QByteArray &bytes) {
        QSaveFile file(root + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save output log export");
    };
    auto p = present(geometry);
    save("geometry.json", QByteArray::fromStdString(geometry.report.dump(2) + "\n"));
    save("geometry-ui.json", QByteArray::fromStdString(p.ui.dump()));
    save("vertices.bin", QByteArray(reinterpret_cast<const char *>(p.values.data()), p.values.size()));
    save("vertices.validity.bin",
         QByteArray(reinterpret_cast<const char *>(p.validity.data()), p.validity.size()));
    for (const auto &[key, filename] :
         {std::pair{"expanded_vertices", "vertices.csv"}, std::pair{"primitives", "primitives.csv"}}) {
        if (!p.ui.at("tables").contains(key))
            continue;
        QByteArray csv;
        auto append = [&](const Json &row) {
            QStringList cells;
            for (const auto &value : row) {
                auto cell =
                    QString::fromStdString(value.is_null()      ? ""
                                           : value.is_boolean() ? (value.get<bool>() ? "True" : "False")
                                           : value.is_string()  ? value.get<std::string>()
                                                                : value.dump());
                if (cell.contains(',') || cell.contains('"') || cell.contains('\n')) {
                    cell.replace("\"", "\"\"");
                    cell = '"' + cell + '"';
                }
                cells.push_back(cell);
            }
            csv += (cells.join(',') + "\r\n").toUtf8();
        };
        const auto &table = p.ui.at("tables").at(key);
        append(table.at("columns"));
        for (const auto &row : table.at("rows"))
            append(row);
        save(filename, csv);
    }
    if (p.ui.at("obj_unavailable_reason").is_null()) {
        std::ostringstream obj;
        obj.imbue(std::locale::classic());
        obj << std::setprecision(9);
        obj << "# Original GS emissions grouped by diagnostic invocation; not a global GPU timeline.\n";
        const auto &mesh = p.ui.at("mesh");
        for (const auto &v : mesh.at("positions"))
            obj << "v " << v[0].get<double>() << ' ' << v[1].get<double>() << ' ' << v[2].get<double>()
                << '\n';
        for (auto [kind, name] : {std::pair{'p', "points"}, std::pair{'l', "lines"}, std::pair{'f', "faces"}})
            for (const auto &primitive : mesh.at(name)) {
                obj << kind;
                if (kind == 'p')
                    obj << ' ' << primitive.get<uint64_t>();
                else
                    for (const auto &index : primitive)
                        obj << ' ' << index.get<uint64_t>();
                obj << '\n';
            }
        save("geometry.obj", QByteArray::fromStdString(obj.str()));
    } else if (QFile::exists(root + "/geometry.obj") && !QFile::remove(root + "/geometry.obj"))
        throw std::runtime_error("Cannot remove obsolete output log OBJ");
}
} // namespace flora
