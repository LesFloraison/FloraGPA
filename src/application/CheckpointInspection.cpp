#include "CheckpointInspection.h"
#include "DxbcCheckpointModel.h"
#include "ShaderSourceLines.h"
#include "SourceStack.h"
#include "SourceVariables.h"
#include "SystemDisassembly.h"
#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <d3dcompiler.h>
#include <iomanip>
#include <sstream>

namespace flora {
namespace {
using Json = nlohmann::json;
uint32_t word(Bytes bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Checkpoint record bounds");
    return Reader(bytes.subspan(offset)).read<uint32_t>();
}
void layout(Bytes data, const Json &metadata) {
    const auto stride = metadata.at("record_stride").get<uint64_t>();
    const auto registers = metadata.at("registers").get<uint64_t>();
    const auto records = metadata.at("records").get<uint64_t>();
    if (registers > 4096 || metadata.at("register_slots").size() != registers ||
        stride != 32 + registers * 32 || records > data.size() / stride || records * stride != data.size())
        throw std::runtime_error("Invalid checkpoint record layout");
}
void write(QSaveFile &file, const std::string &value) {
    if (file.write(value.data(), qint64(value.size())) != qint64(value.size()))
        throw std::runtime_error("Cannot write checkpoint export");
}
void write(const std::filesystem::path &path, Bytes data) {
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char *>(data.data()), qint64(data.size())) != qint64(data.size()) ||
        !file.commit())
        throw std::runtime_error("Cannot save checkpoint export");
}
void write(const std::filesystem::path &path, const std::string &text) {
    write(path, Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
std::string csv(const std::string &value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string result = "\"";
    for (auto c : value) {
        result += c;
        if (c == '"')
            result += c;
    }
    return result + '"';
}
std::string real(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    char text[64];
    const auto converted = std::to_chars(text, text + sizeof text, value);
    if (converted.ec != std::errc{})
        throw std::runtime_error("Cannot format checkpoint value");
    std::string result(text, converted.ptr);
    if (result.find_first_of(".eE") == std::string::npos)
        result += ".0";
    return result;
}
std::string cell(const Json &value) {
    if (value.is_null())
        return {};
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "True" : "False";
    if (value.is_number_float())
        return real(value.get<double>());
    return value.dump();
}
void csvRow(QSaveFile &file, const std::vector<std::string> &values) {
    std::string line;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            line += ',';
        line += csv(values[i]);
    }
    line += "\r\n";
    write(file, line);
}
} // namespace

CheckpointInspection checkpointCatalog(Bytes shader, Id resource, const Json &event,
                                       const CheckpointInspectionOptions &options) {
    auto parsed = checkpoint::program(shader, options.stage);
    CheckpointInspection result;
    result.shader.assign(shader.begin(), shader.end());
    result.assembly = systemDisassembly(shader);
    const QRegularExpression pattern(QStringLiteral("^\\s*(?:(\\d+)\\s+)?0x([0-9a-fA-F]+):\\s*(.*)$"));
    std::map<uint64_t, std::pair<Json, std::string>> lines;
    for (auto line : QString::fromStdString(result.assembly).split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        const auto match = pattern.match(line);
        if (!match.hasMatch())
            continue;
        const auto number = match.captured(1);
        lines[match.captured(2).toULongLong(nullptr, 16)] = {
            number.isEmpty() ? Json(nullptr) : Json(number.toULongLong()), match.captured(3).toStdString()};
    }
    const auto sourceLines = shaderSourceLines(shader, result.assembly);
    auto sourceSymbols = sourceVariables(shader);
    const auto stack = sourceStack(shader, sourceSymbols, sourceLines);
    const auto sourceOffsets = shaderLinesByOffset(sourceLines);
    for (auto &entry : parsed.catalog) {
        const auto found = lines.find(entry.at("word_offset").get<uint64_t>() * 4);
        if (found == lines.end())
            throw std::runtime_error("Disassembler did not map an original shader token offset");
        entry["instruction"] = found->second.first;
        entry["assembly"] = found->second.second;
        entry["checkpoint_allowed"] =
            entry.at("checkpoint_allowed").get<bool>() && !found->second.first.is_null();
        const auto offset = std::to_string(entry.at("word_offset").get<uint64_t>() * 4);
        if (sourceOffsets.contains(offset))
            entry["source_location"] = sourceOffsets.at(offset);
    }
    Json selected = nullptr;
    if (options.instruction) {
        for (const auto &entry : parsed.catalog)
            if (entry.at("instruction") == *options.instruction && entry.at("checkpoint_allowed") == true) {
                selected = entry;
                break;
            }
        if (selected.is_null())
            throw std::runtime_error("Selected shader instruction is not a supported checkpoint");
    }
    Json limits = Json::array({"Values are observed immediately before the selected original instruction, "
                               "not a complete debugger trace.",
                               "Invocation IDs group this diagnostic execution only; they are not a stable "
                               "GPU timeline or DrawInstanceID.",
                               "Written means an assignment was observed, not proof that its source was "
                               "initialized; declared input components are available.",
                               "Emit clears all output register validity; ordinary and indexable temporary "
                               "values remain available.",
                               "Additional logging can change scheduling of original UAV atomics.",
                               "Static CALL/CALLC and indexable temporaries are supported; interface calls "
                               "and feedback instructions are not yet supported.",
                               "Input register displays and invocation identities describe main entry; "
                               "original instructions retain native subroutine input behavior.",
                               "Executed out-of-bounds indexable temporary accesses reject the capture "
                               "because their native behavior is undefined."});
    for (const auto &limit : sourceSymbols.value("limits", Json::array()))
        limits.push_back(limit);
    if (options.stage == "hs") {
        limits[3] = "HS outputs and temporaries describe only the selected original phase execution; "
                    "previous phases are not concatenated.";
        limits.push_back("HS pipeline invocation statistics count patches, while logged entry counts belong "
                         "to the selected control-point/fork/join phase; undeclared phase IDs and draw "
                         "instances remain unknown.");
        limits.push_back("HS source frames are restricted to the selected original phase. Fork/join exclude "
                         "the compiler control-point entry wrapper; missing inline scopes remain unmapped "
                         "instead of falling back to main.");
        limits.push_back(
            "HS single-point and whole-phase logging use identical instrumented bytecode with runtime "
            "selection; driver arithmetic refactoring may still differ from the uninstrumented shader. These "
            "are fresh diagnostic values, not historical GPU register contents.");
    } else if (options.stage == "ds") {
        limits[3] = "DS output registers retain values until overwritten; input values and domain "
                    "coordinates are cached at main entry.";
        limits.push_back("PrimitiveID repeats across draw instances; domain coordinates do not imply a "
                         "unique patch or draw-instance identity.");
    }
    if (options.trace)
        limits[0] = "Snapshots precede executed non-structural instructions in a single diagnostic "
                    "invocation; structural markers have no separate step.";
    result.report = {{"event", event},
                     {"shader", resource},
                     {"shader_sha256", sha256(shader)},
                     {"catalog", parsed.catalog},
                     {"action", options.stage + "-checkpoint"},
                     {"checkpoint", options.instruction ? Json(*options.instruction) : Json(nullptr)},
                     {"backend", "native_d3d11"},
                     {"record_count", 0},
                     {"limits", limits},
                     {"source_lines", sourceLines},
                     {"source_variables", sourceSymbols},
                     {"source_stack", stack},
                     {"source_debug_status", "debugger_configuration_and_qt_pending"}};
    if (options.stage == "hs")
        result.report["hs_phases"] = checkpoint::hullPhases(parsed.code.instructions);
    if (options.trace)
        result.report["trace"] = true;
    if (options.trace || options.instruction)
        result.report["checkpoint_instruction"] = selected;
    return result;
}
Json checkpointRegisters(Bytes data, const Json &metadata, uint32_t record) {
    layout(data, metadata);
    if (record >= metadata.at("records").get<uint32_t>())
        throw std::runtime_error("Checkpoint record index out of bounds");
    const auto base = uint64_t(record) * metadata.at("record_stride").get<uint64_t>() + 32;
    const auto count = metadata.at("registers").get<uint32_t>();
    Json result = Json::array();
    for (uint32_t i = 0; i < count; ++i) {
        Json slot = metadata.at("register_slots").at(i);
        slot["bits"] = Json::array();
        slot["written"] = Json::array();
        for (uint32_t c = 0; c < 4; ++c) {
            slot["bits"].push_back(word(data, base + i * 16 + c * 4));
            const auto flag = word(data, base + count * 16 + i * 16 + c * 4);
            if (flag > 1)
                throw std::runtime_error("Invalid checkpoint register validity");
            slot["written"].push_back(flag == 1);
        }
        result.push_back(std::move(slot));
    }
    return result;
}
Json checkpointHeaders(Bytes data, const Json &metadata) {
    layout(data, metadata);
    const bool trace = metadata.value("trace", false), calls = metadata.contains("call_depth_offset");
    std::map<uint32_t, Json> points;
    if (trace)
        for (const auto &entry : metadata.at("checkpoints"))
            points[entry.at("token")] = entry;
    else
        points[metadata.at("checkpoint").at("token")] = metadata.at("checkpoint");
    Json rows = Json::array();
    const auto count = metadata.at("records").get<uint32_t>(),
               stride = metadata.at("record_stride").get<uint32_t>();
    const auto stage = metadata.at("shader_stage").get<std::string>();
    const auto &known = metadata.at("known_inputs");
    for (uint32_t record = 0; record < count; ++record) {
        const auto base = uint64_t(record) * stride;
        const auto invocation = word(data, base), hit = word(data, base + 4),
                   primitive = word(data, base + 8), instance = word(data, base + 12),
                   token = word(data, base + 16), opcode = word(data, base + 20);
        const auto found = points.find(token);
        if (invocation >= metadata.at("invocations").get<uint32_t>() || found == points.end() ||
            found->second.at("opcode") != opcode)
            throw std::runtime_error("Invalid checkpoint record header");
        Json row{{"record", record},
                 {"invocation", invocation},
                 {"hit", hit},
                 {"primitive_id", known.contains("primitive") ? Json(primitive) : Json(nullptr)},
                 {"gs_instance", known.contains("gs_instance") ? Json(instance) : Json(nullptr)}};
        if (stage == "hs")
            row.update({{"hs_phase", metadata.at("hs_phase").at("id")},
                        {"phase_kind", metadata.at("hs_phase").at("kind")},
                        {"phase_instance", known.contains("hs_instance") ? Json(instance) : Json(nullptr)}});
        if (stage == "ds") {
            const auto registers = checkpointRegisters(data, metadata, record);
            const auto domain = std::find_if(registers.begin(), registers.end(),
                                             [](const auto &r) { return r.at("name") == "vDomain"; });
            Json location = Json::array();
            for (uint32_t c = 0; c < 3; ++c)
                location.push_back(
                    domain != registers.end() && domain->at("written").at(c) == true
                        ? Json(std::bit_cast<float>(domain->at("bits").at(c).template get<uint32_t>()))
                        : Json(nullptr));
            row["domain_location"] = location;
        }
        if (trace)
            row.update({{"token", token}, {"opcode", opcode}});
        if (calls) {
            const auto offset = metadata.at("call_depth_offset").get<uint32_t>();
            if (offset > stride - 4)
                throw std::runtime_error("Checkpoint call depth offset out of bounds");
            const auto depth = word(data, base + offset);
            if (depth > 32)
                throw std::runtime_error("Invalid checkpoint original call depth");
            row["call_depth"] = depth;
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return std::pair{a.at("invocation").template get<uint32_t>(), a.at("hit").template get<uint32_t>()} <
               std::pair{b.at("invocation").template get<uint32_t>(), b.at("hit").template get<uint32_t>()};
    });
    std::optional<uint32_t> current;
    uint32_t expected = 0;
    Json stack = Json::array();
    const Json *previous = nullptr;
    for (auto &row : rows) {
        const auto invocation = row.at("invocation").get<uint32_t>();
        if (!current || *current != invocation) {
            current = invocation;
            expected = 0;
            stack = Json::array();
            previous = nullptr;
        }
        if (row.at("hit") != expected++)
            throw std::runtime_error("Checkpoint has missing or repeated hit ordinals");
        if (trace && calls) {
            const auto depth = row.at("call_depth").get<size_t>();
            const auto &entry = points.at(row.at("token"));
            if (depth == stack.size() + 1 && previous) {
                const auto &caller = points.at(previous->at("token"));
                if ((caller.at("opcode") != 4 && caller.at("opcode") != 5) || !caller.contains("call_target"))
                    throw std::runtime_error("Trace entered a subroutine without a call");
                stack.push_back({{"label", caller.at("call_target")}, {"call_token", previous->at("token")}});
            } else if (!stack.empty() && depth == stack.size() - 1 && previous &&
                       (previous->at("opcode") == 62 || previous->at("opcode") == 63))
                stack.erase(stack.end() - 1);
            else if (depth != stack.size())
                throw std::runtime_error("Trace has inconsistent original call depth");
            if (entry.value("function_label", Json(nullptr)) !=
                (stack.empty() ? Json(nullptr) : stack.back().at("label")))
                throw std::runtime_error("Trace subroutine does not match the original call target");
            row["call_stack"] = stack;
            previous = &row;
        }
    }
    return rows;
}
void completeCheckpointInspection(CheckpointInspection &inspection, Json metadata,
                                  std::vector<uint8_t> records) {
    auto rows = checkpointHeaders(records, metadata);
    if (metadata.contains("input_selector")) {
        std::set<uint32_t> groups;
        for (const auto &row : rows)
            groups.insert(row.at("invocation").get<uint32_t>());
        if (groups.size() != metadata.at("matched_invocations").get<size_t>())
            throw std::runtime_error("Selected trace does not contain every matched invocation");
        inspection.report["limits"].push_back(
            "Input selection matches original declared main-input bits in this fresh execution; it is not a "
            "persistent invocation identity or proof of historical values.");
    }
    if (metadata.value("trace", false)) {
        std::map<uint32_t, Json> instructions;
        for (const auto &entry : inspection.report.at("catalog"))
            instructions[entry.at("token")] = entry.at("instruction");
        for (auto &row : rows) {
            row["instruction"] = instructions.at(row.at("token"));
            if (row.contains("call_stack"))
                for (auto &frame : row["call_stack"])
                    frame["call_instruction"] = instructions.at(frame.at("call_token"));
        }
    }
    inspection.report["record_count"] = rows.size();
    inspection.report["register_capture"] = std::move(metadata);
    inspection.report["preview_truncated"] = rows.size() > 10000;
    if (rows.size() > 10000)
        rows.erase(rows.begin() + 10000, rows.end());
    inspection.report["hits_preview"] = std::move(rows);
    inspection.bytes = std::move(records);
}
void exportCheckpoint(const CheckpointInspection &inspection, const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
    write(directory / "shader.asm", inspection.assembly);
    write(directory / "shader.dxbc", inspection.shader);
    if (inspection.report.contains("register_capture")) {
        const auto &meta = inspection.report.at("register_capture");
        auto rows = checkpointHeaders(inspection.bytes, meta);
        write(directory / "snapshots.bin", inspection.bytes);
        const auto stage = meta.at("shader_stage").get<std::string>();
        std::vector<std::string> keys{"record", "invocation", "hit", "primitive_id"}, extra;
        if (stage == "gs")
            keys.push_back("gs_instance");
        else if (stage == "hs")
            keys.insert(keys.end(), {"hs_phase", "phase_kind", "phase_instance"});
        if (meta.value("trace", false))
            extra = {"token", "instruction"};
        if (meta.contains("call_depth_offset"))
            extra.push_back("call_depth");
        auto heading = keys;
        heading.push_back("draw_instance");
        if (stage == "ds")
            heading.insert(heading.end(), {"domain.u", "domain.v", "domain.w"});
        heading.insert(heading.end(), extra.begin(), extra.end());
        QSaveFile hits(QString::fromStdWString((directory / "hits.csv").wstring()));
        QSaveFile registers(QString::fromStdWString((directory / "registers.csv").wstring()));
        if (!hits.open(QIODevice::WriteOnly) || !registers.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot create checkpoint CSV exports");
        csvRow(hits, heading);
        csvRow(registers, {"record", "register", "component", "written", "hex", "uint", "int", "float"});
        std::map<uint32_t, Json> instructions;
        for (const auto &entry : inspection.report.at("catalog"))
            instructions[entry.at("token")] = entry.at("instruction");
        for (auto &row : rows) {
            if (meta.value("trace", false))
                row["instruction"] = instructions.at(row.at("token"));
            std::vector<std::string> values;
            for (const auto &key : keys)
                values.push_back(cell(row.at(key)));
            values.push_back(meta.at("instance_count") == 1 ? "0" : "");
            if (stage == "ds")
                for (const auto &component : row.at("domain_location"))
                    values.push_back(cell(component));
            for (const auto &key : extra)
                values.push_back(cell(row.at(key)));
            csvRow(hits, values);
            for (const auto &reg : checkpointRegisters(inspection.bytes, meta, row.at("record")))
                for (uint32_t c = 0; c < 4; ++c) {
                    const auto bits = reg.at("bits").at(c).get<uint32_t>();
                    const bool written = reg.at("written").at(c);
                    std::vector<std::string> cells{cell(row.at("record")), reg.at("name"),
                                                   std::string(1, "xyzw"[c]), written ? "True" : "False"};
                    if (written) {
                        std::ostringstream hex;
                        hex << "0x" << std::hex << std::setw(8) << std::setfill('0') << bits;
                        cells.insert(cells.end(), {hex.str(), std::to_string(bits),
                                                   std::to_string(std::bit_cast<int32_t>(bits)),
                                                   real(std::bit_cast<float>(bits))});
                    } else
                        cells.insert(cells.end(), 4, "");
                    csvRow(registers, cells);
                }
        }
        if (!hits.commit() || !registers.commit())
            throw std::runtime_error("Cannot save checkpoint CSV exports");
    }
    write(directory / "checkpoint.json", inspection.report.dump(2));
}
} // namespace flora
