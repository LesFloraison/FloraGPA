#include "Geometry.h"
#include "StreamOutputInspector.h"
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>

namespace flora {
using Json = nlohmann::json;
namespace {
Json number(double x) {
    if (std::isnan(x))
        return "nan";
    if (std::isinf(x))
        return x < 0 ? "-inf" : "inf";
    return x;
}
double miniFloat(uint32_t bits, unsigned fractionBits) {
    auto exponent = bits >> fractionBits, fraction = bits & ((1u << fractionBits) - 1);
    if (exponent == 31)
        return fraction ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    return exponent
               ? std::ldexp(double((1u << fractionBits) + fraction), int(exponent) - 15 - int(fractionBits))
               : std::ldexp(double(fraction), -14 - int(fractionBits));
}
Bytes range(Bytes data, uint64_t offset, uint64_t count) {
    if (offset > data.size() || count > data.size() - offset)
        throw std::runtime_error("Vertex or index fetch exceeds buffer bounds");
    return data.subspan(size_t(offset), size_t(count));
}
void save(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Cannot export geometry");
}
QString cell(const Json &value) {
    if (value.is_null())
        return {};
    if (value.is_string())
        return QString::fromStdString(value.get<std::string>());
    if (value.is_boolean())
        return value.get<bool>() ? "True" : "False";
    return QString::fromStdString(value.dump());
}
QString csvCell(const Json &value) {
    auto text = cell(value);
    if (text.contains(',') || text.contains('"') || text.contains('\n') || text.contains('\r')) {
        text.replace('"', "\"\"");
        text = '"' + text + '"';
    }
    return text;
}
} // namespace
Json vertexValue(Bytes data, uint64_t offset, uint32_t format) {
    struct Family {
        std::vector<uint32_t> formats;
        std::string codes;
        unsigned count;
    };
    static const std::vector<Family> families{
        {{2, 3, 4}, "fIi", 4},        {{6, 7, 8}, "fIi", 3},         {{10, 11, 12, 13, 14}, "eHHhh", 4},
        {{16, 17, 18}, "fIi", 2},     {{28, 30, 31, 32}, "BBbb", 4}, {{34, 35, 36, 37, 38}, "eHHhh", 2},
        {{41, 42, 43}, "fIi", 1},     {{49, 50, 51, 52}, "BBbb", 2}, {{54, 56, 57, 58, 59}, "eHHhh", 1},
        {{61, 62, 63, 64}, "BBbb", 1}};
    for (const auto &family : families) {
        auto it = std::find(family.formats.begin(), family.formats.end(), format);
        if (it == family.formats.end())
            continue;
        auto code = family.codes[size_t(it - family.formats.begin())];
        auto size = (code == 'B' || code == 'b') ? 1u : (code == 'e' || code == 'H' || code == 'h') ? 2u : 4u;
        Reader r(range(data, offset, size * family.count));
        Json result = Json::array();
        for (unsigned i = 0; i < family.count; ++i) {
            Json value;
            switch (code) {
            case 'f':
                value = number(r.read<float>());
                break;
            case 'I':
                value = r.read<uint32_t>();
                break;
            case 'i':
                value = r.read<int32_t>();
                break;
            case 'H':
                value = r.read<uint16_t>();
                break;
            case 'h':
                value = r.read<int16_t>();
                break;
            case 'B':
                value = r.read<uint8_t>();
                break;
            case 'b':
                value = r.read<int8_t>();
                break;
            default: {
                auto h = r.read<uint16_t>();
                value = number((h & 0x8000 ? -1. : 1.) * miniFloat(h & 0x7fff, 10));
                break;
            }
            }
            if (format == 11 || format == 28 || format == 35 || format == 49 || format == 56 || format == 61)
                value = value.get<double>() / (code == 'B' ? 255. : 65535.);
            if (format == 13 || format == 31 || format == 37 || format == 51 || format == 58 || format == 63)
                value = std::max(-1., value.get<double>() / (code == 'b' ? 127. : 32767.));
            result.push_back(value);
        }
        return result;
    }
    if (format == 24 || format == 25) {
        Reader r(range(data, offset, 4));
        auto v = r.read<uint32_t>();
        Json values = {v & 1023, (v >> 10) & 1023, (v >> 20) & 1023, v >> 30};
        if (format == 24)
            for (size_t i = 0; i < 4; ++i)
                values[i] = values[i].get<double>() / (i == 3 ? 3. : 1023.);
        return values;
    }
    if (format == 26) {
        Reader r(range(data, offset, 4));
        auto v = r.read<uint32_t>();
        return {number(miniFloat(v & 2047, 6)), number(miniFloat((v >> 11) & 2047, 6)),
                number(miniFloat(v >> 22, 5))};
    }
    if (format == 87 || format == 88) {
        Reader r(range(data, offset, 4));
        auto b = r.read<uint8_t>(), g = r.read<uint8_t>(), red = r.read<uint8_t>(), a = r.read<uint8_t>();
        return {red / 255., g / 255., b / 255., format == 87 ? a / 255. : 1.};
    }
    if (format == 85 || format == 86 || format == 115) {
        Reader r(range(data, offset, 2));
        auto v = r.read<uint16_t>();
        if (format == 85)
            return {((v >> 11) & 31) / 31., ((v >> 5) & 63) / 63., (v & 31) / 31.};
        if (format == 86)
            return {((v >> 10) & 31) / 31., ((v >> 5) & 31) / 31., (v & 31) / 31., double(v >> 15)};
        return {((v >> 8) & 15) / 15., ((v >> 4) & 15) / 15., (v & 15) / 15., (v >> 12) / 15.};
    }
    throw std::runtime_error("Unsupported vertex DXGI format " + std::to_string(format));
}
Json meshPrimitives(const std::vector<int64_t> &stream, uint32_t topology) {
    Json result = {{"points", Json::array()}, {"lines", Json::array()}, {"faces", Json::array()}};
    std::vector<int64_t> values;
    auto flush = [&] {
        auto n = values.size();
        if (topology == 1)
            for (auto v : values)
                result["points"].push_back(v);
        else if (topology == 2 || topology == 10) {
            size_t step = topology == 2 ? 2 : 4, offset = topology == 2 ? 0 : 1;
            for (size_t i = 0; i + step <= n; i += step)
                result["lines"].push_back({values[i + offset], values[i + offset + 1]});
        } else if (topology == 3 || topology == 11) {
            size_t skip = topology == 3 ? 0 : 1;
            for (size_t i = skip; i + 1 + skip < n; ++i)
                result["lines"].push_back({values[i], values[i + 1]});
        } else if (topology == 4 || topology == 12) {
            size_t step = topology == 4 ? 3 : 6, gap = topology == 4 ? 1 : 2;
            for (size_t i = 0; i + step <= n; i += step)
                result["faces"].push_back({values[i], values[i + gap], values[i + gap * 2]});
        } else if (topology == 5 || topology == 13) {
            size_t size = topology == 5 ? 3 : 6, step = topology == 5 ? 1 : 2;
            for (size_t i = 0, ordinal = 0; i + size <= n; i += step, ++ordinal) {
                auto a = values[i], b = values[i + step], c = values[i + 2 * step];
                if (ordinal % 2)
                    std::swap(a, b);
                if (a != b && b != c && a != c)
                    result["faces"].push_back({a, b, c});
            }
        }
        values.clear();
    };
    for (auto v : stream) {
        if (v == 0)
            flush();
        else
            values.push_back(v);
    }
    flush();
    return result;
}
Json inspectGeometry(const Frame &, Replay &replay, Id eventId) {
    const auto &frame = replay.frame();
    auto event = frame.event(eventId);
    if (event.type < 0x37 || event.type > 0x3d)
        throw std::runtime_error("IA geometry requires a supported graphics draw");
    auto state = effectiveBindings(frame, eventId, frame.state(event.state), replay.options());
    if (!state.layout)
        throw std::runtime_error("This procedural draw has no IA input layout");
    Reader resource(frame.payload(state.layout, 5, 0x82));
    resource.skip(16);
    auto layoutId = resource.read<Id>();
    resource.end();
    Reader layout(frame.payload(layoutId, 9, 0x84));
    auto n = layout.read<uint32_t>();
    if (n > 32)
        throw std::runtime_error("Input layout exceeds 32 elements");
    struct Element {
        std::string name;
        uint32_t format, slot, offset, classification, step;
    };
    std::vector<Element> elements;
    std::map<uint32_t, uint64_t> offsets;
    std::optional<size_t> positionElement;
    Json metadata = Json::array(), columns = {"instance", "draw_vertex", "vertex_index", "strip_restart"};
    std::vector<size_t> components;
    size_t totalComponents = 0;
    for (uint32_t i = 0; i < n; ++i) {
        auto name = frame.data(layout.read<Id>());
        if (name.empty() || name.back() != 0)
            throw std::runtime_error("Invalid input semantic");
        auto index = layout.read<uint32_t>();
        Element e{QString::fromUtf8(reinterpret_cast<const char *>(name.data()), qsizetype(name.size() - 1))
                          .toStdString() +
                      std::to_string(index),
                  layout.read<uint32_t>(),
                  layout.read<uint32_t>(),
                  layout.read<uint32_t>(),
                  layout.read<uint32_t>(),
                  layout.read<uint32_t>()};
        if (e.slot >= 32 || e.classification > 1)
            throw std::runtime_error("Invalid IA slot or classification");
        auto size = pitches(1, 1, e.format).first;
        std::vector<uint8_t> zero(size);
        auto count = vertexValue(zero, 0, e.format).size();
        if (e.offset == UINT32_MAX) {
            auto align = std::min(4u, size);
            auto offset = (offsets[e.slot] + align - 1) / align * align;
            if (offset > UINT32_MAX)
                throw std::runtime_error("Append offset overflow");
            e.offset = uint32_t(offset);
        }
        offsets[e.slot] = uint64_t(e.offset) + size;
        elements.push_back(e);
        components.push_back(count);
        totalComponents += count;
        for (size_t k = 0; k < count; ++k)
            columns.push_back(e.name + "." + std::to_string(k));
        if (!positionElement && QString::fromStdString(e.name).toUpper().startsWith("POSITION"))
            positionElement = i;
        metadata.push_back({{"name", e.name},
                            {"format", e.format},
                            {"slot", e.slot},
                            {"offset", e.offset},
                            {"classification", e.classification},
                            {"step", e.step}});
    }
    layout.skip(layout.read<uint32_t>());
    layout.end();
    auto args = event.args;
    Json automatic = nullptr;
    if (event.type == 0x38) {
        auto parameters = replay.drawAutoParameters(eventId);
        args = {parameters.vertexCount, 0};
        automatic = drawAutoJson(parameters);
    }
    bool indexed = event.type == 0x39 || event.type == 0x3a || event.type == 0x3b;
    Json indirect = nullptr;
    if (event.argumentBuffer) {
        auto raw = replay.readBuffer(event.argumentBuffer);
        auto offset = args.at(0);
        if (offset % 4)
            throw std::runtime_error("Unaligned indirect arguments");
        auto bytes = range(raw, offset, indexed ? 20 : 16);
        Reader r(bytes);
        args.clear();
        while (r.remaining())
            args.push_back(r.read<uint32_t>());
        indirect = {
            {"resource", std::to_string(event.argumentBuffer)},
            {"offset", offset},
            {"size", bytes.size()},
            {"raw_hex", QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()))
                            .toHex()
                            .toStdString()}};
    }
    bool instanced = event.type == 0x3a || event.type == 0x3b || event.type == 0x3c || event.type == 0x3d;
    uint32_t count = args.at(0), instances = instanced ? args.at(1) : 1, start = args.at(instanced ? 2 : 1),
             startInstance = instanced ? args.at(indexed ? 4 : 3) : 0;
    int32_t base = indexed ? int32_t(args.at(instanced ? 3 : 2)) : 0;
    auto refs = uint64_t(count) * instances;
    if (refs > 1000000 || refs * (totalComponents + 4) > 16000000)
        throw std::runtime_error("IA table exceeds the current native inspection limit");
    Json params = {{indexed ? "index_count" : "vertex_count", count}};
    params[indexed ? "start_index" : "start_vertex"] = start;
    if (indexed)
        params["base_vertex"] = base;
    if (instanced) {
        params["instance_count"] = instances;
        params["start_instance"] = startInstance;
    }
    std::vector<std::optional<int64_t>> vertices;
    std::vector<uint32_t> indices;
    bool strips = state.topology == 3 || state.topology == 5 || state.topology == 11 || state.topology == 13;
    if (refs && indexed) {
        auto raw = replay.readBuffer(state.ib);
        auto size = state.ibFormat == 42 ? 4u : state.ibFormat == 57 ? 2u : 0u;
        if (!size)
            throw std::runtime_error("Index format must be R16_UINT or R32_UINT");
        Reader r(range(raw, uint64_t(state.ibOffset) + uint64_t(start) * size, uint64_t(count) * size));
        for (uint32_t i = 0; i < count; ++i) {
            auto v = size == 4 ? r.read<uint32_t>() : r.read<uint16_t>();
            indices.push_back(v);
            if (strips && v == (size == 4 ? UINT32_MAX : UINT16_MAX))
                vertices.push_back(std::nullopt);
            else
                vertices.push_back(int64_t(v) + base);
        }
    } else if (refs)
        for (uint32_t i = 0; i < count; ++i)
            vertices.push_back(int64_t(start) + i);
    std::map<uint32_t, std::vector<uint8_t>> buffers;
    for (const auto &e : elements)
        if (!buffers.contains(e.slot)) {
            if (!state.vb[e.slot])
                throw std::runtime_error("Missing vertex buffer");
            buffers[e.slot] = replay.readBuffer(state.vb[e.slot]);
        }
    Json rows = Json::array(), unique = Json::array(), references = Json::array(), positions = Json::array(),
         points = Json::array(), lines = Json::array(), faces = Json::array();
    uint64_t uniqueCount = 0, restarts = 0;
    for (uint32_t instance = 0; !vertices.empty() && instance < instances; ++instance) {
        std::map<int64_t, uint64_t> identities;
        std::vector<int64_t> stream;
        for (size_t ordinal = 0; ordinal < vertices.size(); ++ordinal) {
            Json index = indexed ? Json(indices[ordinal]) : Json(nullptr);
            if (!vertices[ordinal]) {
                Json row = {instance, ordinal, nullptr, true};
                for (size_t k = 0; k < totalComponents; ++k)
                    row.push_back(nullptr);
                rows.push_back(row);
                references.push_back({instance, ordinal, index, nullptr, true, nullptr});
                ++restarts;
                stream.push_back(0);
                continue;
            }
            auto vertex = *vertices[ordinal];
            if (vertex < 0)
                throw std::runtime_error("Negative IA vertex fetch");
            Json attributes = Json::array();
            std::vector<Json> grouped;
            for (const auto &e : elements) {
                auto element = e.classification ? uint64_t(startInstance) + (e.step ? instance / e.step : 0)
                                                : uint64_t(vertex);
                auto offset = uint64_t(state.offsets[e.slot]) + e.offset;
                if (state.strides[e.slot] && element > (UINT64_MAX - offset) / state.strides[e.slot])
                    throw std::runtime_error("IA address overflow");
                offset += element * state.strides[e.slot];
                auto values = vertexValue(buffers.at(e.slot), offset, e.format);
                grouped.push_back(values);
                for (const auto &value : values)
                    attributes.push_back(value);
            }
            Json row = {instance, ordinal, vertex, false};
            for (auto &value : attributes)
                row.push_back(value);
            rows.push_back(row);
            auto [it, inserted] = identities.emplace(vertex, uniqueCount);
            if (inserted) {
                ++uniqueCount;
                Json u = {it->second, instance, ordinal, vertex};
                for (auto &v : attributes)
                    u.push_back(v);
                unique.push_back(u);
                if (positionElement) {
                    auto p = grouped[*positionElement];
                    Json xyz = Json::array();
                    for (size_t k = 0; k < 3; ++k)
                        xyz.push_back(k < p.size() ? p[k] : Json(0.));
                    positions.push_back(xyz);
                }
            }
            references.push_back({instance, ordinal, index, vertex, false, it->second});
            if (positionElement)
                stream.push_back(int64_t(it->second) + 1);
        }
        auto primitives = meshPrimitives(stream, state.topology);
        for (auto &x : primitives["points"])
            points.push_back(x);
        for (auto &x : primitives["lines"])
            lines.push_back(x);
        for (auto &x : primitives["faces"])
            faces.push_back(x);
    }
    Json uniqueColumns = {"unique_vertex", "instance", "first_draw_vertex", "vertex_index"};
    for (size_t k = 4; k < columns.size(); ++k)
        uniqueColumns.push_back(columns[k]);
    Json result = {
        {"event", std::to_string(eventId)},
        {"value_time", "immediately_before_draw"},
        {"geometry_stage", "IA_input"},
        {"attribute_interpretation", "decoded_buffer_storage"},
        {"effective_parameters", params},
        {"indirect_arguments", indirect},
        {"draw_auto", automatic},
        {"topology", state.topology},
        {"elements", metadata},
        {"vertex_references", refs},
        {"unique_vertices", uniqueCount},
        {"strip_restart_references", restarts},
        {"obj_vertices", positions.size()},
        {"obj_faces", faces.size()},
        {"obj_lines", lines.size()},
        {"obj_points", points.size()},
        {"tables",
         {{"expanded_vertices", {{"columns", columns}, {"rows", rows}}},
          {"unique_vertices", {{"columns", uniqueColumns}, {"rows", unique}}},
          {"references",
           {{"columns",
             {"instance", "draw_vertex", "index_value", "vertex_index", "strip_restart", "unique_vertex"}},
            {"rows", references}}}}},
        {"mesh", {{"positions", positions}, {"points", points}, {"lines", lines}, {"faces", faces}}},
        {"limitations", Json::array({"IA storage values precede shader transformations."})}};
    if (state.topology >= 33 && state.topology <= 64)
        result["limitations"].push_back("Patch control points are not tessellated surfaces.");
    for (const auto &e : elements)
        if (e.format == 85 || e.format == 86 || e.format == 115) {
            result["limitations"].push_back(
                "Packed BGR storage values are not validated against native IA fetch.");
            break;
        }
    return result;
}
void exportGeometry(const Json &geometry, const QString &directory) {
    save(directory + "/geometry.json", QByteArray::fromStdString(geometry.dump(2)));
    for (const auto &[table, file] :
         std::map<std::string, QString>{{"expanded_vertices", "vertices.csv"},
                                        {"unique_vertices", "unique_vertices.csv"},
                                        {"references", "references.csv"}}) {
        const auto &data = geometry.at("tables").at(table);
        QByteArray csv;
        auto append = [&](const Json &values) {
            QStringList cells;
            for (const auto &v : values)
                cells << csvCell(v);
            csv += (cells.join(',') + '\n').toUtf8();
        };
        append(data.at("columns"));
        for (auto &row : data.at("rows"))
            append(row);
        save(directory + '/' + file, csv);
    }
    const auto &mesh = geometry.at("mesh");
    if (mesh.at("positions").empty())
        return;
    QByteArray obj = "# IA positions before shaders; instance transforms are not applied.\n";
    for (const auto &p : mesh.at("positions")) {
        obj += "v";
        for (const auto &v : p)
            obj += ' ' + cell(v).toUtf8();
        obj += '\n';
    }
    for (const auto &[kind, prefix] : std::map<std::string, QByteArray>{{"faces", "f"}, {"lines", "l"}})
        for (auto &primitive : mesh.at(kind)) {
            obj += prefix;
            for (auto &v : primitive)
                obj += ' ' + cell(v).toUtf8();
            obj += '\n';
        }
    for (auto &point : mesh.at("points"))
        obj += "p " + cell(point).toUtf8() + '\n';
    save(directory + "/geometry.obj", obj);
}
} // namespace flora
