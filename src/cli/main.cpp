#include "application/ApiCommands.h"
#include "application/CaptureNames.h"
#include "application/ClassInspector.h"
#include "application/CommandState.h"
#include "application/Constants.h"
#include "application/ContextInspector.h"
#include "application/Experiment.h"
#include "application/Geometry.h"
#include "application/PredicateInspector.h"
#include "application/ReplayPipeline.h"
#include "application/ShaderInspector.h"
#include "application/StreamOutputInspector.h"
#include "application/UavCounterInspector.h"
#include "core/BufferBindings.h"
#include "core/Frame.h"
#include "replay/Replay.h"
#include <Psapi.h>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>
#include <d3dcompiler.h>

using namespace flora;
void save(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Cannot save " + path.toStdString());
}
QString digest(const QByteArray &data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
QJsonArray modules() {
    HMODULE handles[1024];
    DWORD bytes = 0;
    QJsonArray out;
    if (EnumProcessModules(GetCurrentProcess(), handles, sizeof handles, &bytes)) {
        for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i) {
            wchar_t path[32768]{};
            if (GetModuleFileNameW(handles[i], path, 32768))
                out.append(QString::fromWCharArray(path));
        }
    }
    return out;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("FloraGPA.Cli");
    app.setApplicationVersion("0.1.0");
    QCommandLineParser p;
    p.setApplicationDescription("Native DX11 capture inspection and isolated replay");
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument(
        "command", "inventory | commands | command-state | contexts | command-lists | replay | shader | "
                   "buffer | texture | compile | geometry | replay-pipeline | class-linkage | predicate");
    p.addPositionalArgument("capture", "DX11 .gpa_frame file");
    p.addOption({"out", "New or empty output directory", "path"});
    p.addOption({"experiment", "Compatible FloraGPA experiment project", "path"});
    p.addOption({"source", "HLSL translation unit", "path"});
    p.addOption({"entry", "HLSL entry point", "name", "main"});
    p.addOption({"warp", "Use the WARP software adapter"});
    p.addOption({"timings", "Collect native GPU timestamps and pipeline statistics"});
    p.addOption({"debug-device", "Enable D3D11 validation"});
    p.addOption({"event", "Stop at API event", "id"});
    p.addOption({"before", "Stop before the selected event"});
    p.addOption({"id", "Resource ID", "id"});
    p.addOption({"filter", "API command text filter", "text"});
    p.addOption({"resource", "Filter API commands by referenced resource", "id"});
    p.addOption({"subresource", "Texture subresource", "index", "0"});
    p.addOption({"mip", "Texture mip level", "index", "0"});
    p.addOption({"layer", "Texture array layer", "index", "0"});
    p.addOption({"slice", "Texture depth slice", "index", "0"});
    p.addOption({"offset", "Buffer byte offset", "bytes", "0"});
    p.addOption({"length", "Buffer byte length (default: remaining bytes)", "bytes"});
    p.addOption({"suppress-draws", "Disable draw submissions (negative control)"});
    p.addOption({"disable", "Comma-separated disabled event IDs", "ids"});
    p.process(app);
    try {
        const auto args = p.positionalArguments();
        if (args.size() != 2)
            throw std::runtime_error("Expected command and capture path");
        Frame frame(std::filesystem::path(args[1].toStdWString()));
        auto command = args[0];
        auto parseId = [&](const QString &key) {
            bool valid = false;
            auto id = p.value(key).toULongLong(&valid);
            if (!valid)
                throw std::runtime_error("Invalid unsigned identifier: " + key.toStdString());
            return Id(id);
        };
        auto parseIndex = [&](const QString &key) {
            auto value = parseId(key);
            if (value > UINT32_MAX)
                throw std::runtime_error("Index exceeds uint32 range");
            return UINT(value);
        };
        if (p.isSet("before") && !p.isSet("event"))
            throw std::runtime_error("--before requires --event");
        if (p.isSet("event") && (!parseId("event") || frame.entry(parseId("event")).category != 7))
            throw std::runtime_error("Event must identify an API command");
        if (p.isSet("experiment") && p.isSet("disable"))
            throw std::runtime_error("Use experiment operations or --disable, not both");
        QJsonObject report{{"schema", "FloraGPA native result 1"},
                           {"capture", args[1]},
                           {"width", int(frame.width())},
                           {"height", int(frame.height())},
                           {"entries", int(frame.entries().size())},
                           {"reference_pixels_used", false}};
        nlohmann::json constantBindings;
        QString out = p.value("out");
        if (!out.isEmpty()) {
            QDir dir(out);
            if (dir.exists() &&
                !dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).empty())
                throw std::runtime_error("Output directory must be new or empty");
            if (!QDir().mkpath(out))
                throw std::runtime_error("Cannot create output directory");
        }
        if (command == "class-linkage") {
            if (out.isEmpty() || !p.isSet("id"))
                throw std::runtime_error("class-linkage requires --id and --out");
            auto detail = inspectClass(frame, parseId("id"));
            save(out + "/class-linkage.json", QByteArray::fromStdString(detail.dump(2) + "\n"));
            report.insert("completed", true);
        } else if (command == "command-state") {
            if (out.isEmpty() || !p.isSet("event"))
                throw std::runtime_error("command-state requires --event and --out");
            auto detail = inspectCommandState(frame, parseId("event"), !p.isSet("before"));
            save(out + "/command-state.json", QByteArray::fromStdString(detail.dump(2) + "\n"));
            report.insert("completed", true);
        } else if (command == "contexts" || command == "command-lists") {
            if (out.isEmpty())
                throw std::runtime_error("Context inspection requires --out");
            auto detail = command == "contexts" ? inspectContexts(frame) : inspectCommandLists(frame);
            save(out + '/' + command + ".json", QByteArray::fromStdString(detail.dump(2) + "\n"));
            report.insert("completed", true);
        } else if (command == "commands") {
            if (out.isEmpty())
                throw std::runtime_error("commands requires --out");
            exportCommands(frame, std::filesystem::path(out.toStdWString()), p.value("filter").toStdString(),
                           p.isSet("resource") ? std::optional<Id>(parseId("resource")) : std::nullopt);
            report.insert("completed", true);
        } else if (command == "inventory") {
            auto names = capturedNames(frame);
            report.insert("debug_names",
                          QJsonDocument::fromJson(QByteArray::fromStdString(names.dump())).object());
            QJsonArray events, resources;
            uint64_t draws = 0, dispatches = 0;
            for (const auto &[id, e] : frame.entries()) {
                if (e.category == 7) {
                    QJsonObject row{{"id", QString::number(id)},
                                    {"name", QString::fromStdString(commandName(e.type))},
                                    {"type", e.type},
                                    {"bytes", int(e.size)}};
                    if (isDraw(e.type)) {
                        auto ev = frame.event(id);
                        row.insert("state", QString::number(ev.state));
                        row.insert("context", QString::number(ev.context));
                        QJsonArray a;
                        for (auto x : ev.args)
                            a.append(qint64(x));
                        row.insert("parameters", a);
                        if (e.type == 0x35 || e.type == 0x36)
                            ++dispatches;
                        else
                            ++draws;
                    }
                    events.append(row);
                }
                if (e.category == 5) {
                    QJsonObject row{{"id", QString::number(id)},
                                    {"name", QString::fromStdString(resourceName(e.type))},
                                    {"type", e.type},
                                    {"bytes", int(e.size)}};
                    resources.append(row);
                }
            }
            report.insert("commands", events);
            report.insert("resources", resources);
            report.insert("draws", qint64(draws));
            report.insert("dispatches", qint64(dispatches));
        } else if (command == "compile") {
            if (out.isEmpty() || !p.isSet("id") || !p.isSet("source"))
                throw std::runtime_error("compile requires --id, --source and --out");
            auto resource = frame.resource(parseId("id"));
            auto info = inspectShader(frame.shader(resource.data));
            auto profile = info.at("profile").get<std::string>();
            QFile source(p.value("source"));
            if (!source.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read HLSL source");
            auto bytes = source.readAll();
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
            auto lines = bytes.split('\n');
            for (qsizetype i = 0; i < std::min<qsizetype>(5, lines.size()); ++i)
                if (lines[i].trimmed() == "// FloraGPA compiler optimization: preserve")
                    flags = D3DCOMPILE_SKIP_OPTIMIZATION;
            Com<ID3DBlob> code, errors;
            auto entry = p.value("entry").toUtf8();
            auto hr = D3DCompile(bytes.data(), size_t(bytes.size()), "edited.hlsl", nullptr, nullptr,
                                 entry.constData(), profile.c_str(), flags, 0, &code, &errors);
            QByteArray diagnostics;
            if (errors)
                diagnostics = QByteArray(static_cast<const char *>(errors->GetBufferPointer()),
                                         qsizetype(errors->GetBufferSize()));
            save(out + "/compiler.log", diagnostics);
            if (FAILED(hr))
                throw std::runtime_error(diagnostics.isEmpty() ? "HLSL compilation failed"
                                                               : diagnostics.toStdString());
            save(out + "/replacement.dxbc", QByteArray(static_cast<const char *>(code->GetBufferPointer()),
                                                       qsizetype(code->GetBufferSize())));
            report.insert("completed", true);
            report.insert("profile", QString::fromStdString(profile));
        } else if (command == "shader") {
            if (!p.isSet("id") || out.isEmpty())
                throw std::runtime_error("shader requires --id and --out");
            auto res = frame.resource(parseId("id"));
            auto bytes = frame.shader(res.data);
            save(out + "/shader.dxbc",
                 QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
            save(out + "/shader.asm", QByteArray::fromStdString(disassemble(bytes)));
            auto metadata = inspectResourceShader(frame, parseId("id"), bytes);
            save(out + "/shader.json", QByteArray::fromStdString(metadata.dump(2)));
            const auto &sources = metadata["embedded_sources"]["files"];
            if (!sources.empty()) {
                QDir().mkpath(out + "/shader_sources");
                for (size_t i = 0; i < sources.size(); ++i)
                    save(out + QString("/shader_sources/%1.hlsl").arg(i, 4, 10, QChar('0')),
                         QByteArray::fromHex(
                             QByteArray::fromStdString(sources[i]["raw_hex"].get<std::string>())));
            }
            report.insert("id", p.value("id"));
        } else if (command == "replay" || command == "buffer" || command == "texture" ||
                   command == "geometry" || command == "replay-pipeline" || command == "predicate") {
            if (out.isEmpty())
                throw std::runtime_error("Replay requires --out");
            ReplayOptions options;
            options.timings = p.isSet("timings");
            options.warp = p.isSet("warp");
            options.debug = p.isSet("debug-device");
            options.suppressDraws = p.isSet("suppress-draws");
            options.before = p.isSet("before");
            if (p.isSet("event"))
                options.until = parseId("event");
            if (command == "predicate" && (!options.until || !p.isSet("id")))
                throw std::runtime_error("predicate requires --event and --id");
            if (command == "geometry") {
                if (!options.until)
                    throw std::runtime_error("geometry requires --event");
                options.before = true;
            }
            for (auto x : p.value("disable").split(',', Qt::SkipEmptyParts)) {
                bool ok;
                auto id = x.toULongLong(&ok);
                if (!ok || frame.entry(id).category != 7)
                    throw std::runtime_error("Invalid disabled event");
                options.disabled.insert(id);
            }
            if (p.isSet("experiment")) {
                Experiment project(frame);
                project.load(p.value("experiment"), frame);
                project.apply(frame, options);
            }
            Replay replay(frame, options);
            report.insert("adapter", QString::fromStdString(replay.adapter()));
            report.insert("event", options.until ? QJsonValue(QString::number(options.until))
                                                 : QJsonValue(QJsonValue::Null));
            report.insert("value_time",
                          options.until ? (options.before ? "before_event" : "after_event") : "frame_end");
            auto progress = [](Id event, size_t done, size_t total) {
                QTextStream(stderr) << "progress " << event << ' ' << done << ' ' << total << Qt::endl;
            };
            if (command == "replay-pipeline") {
                auto pipeline = inspectReplayPipeline(frame, replay, p.isSet("experiment"), progress);
                save(out + "/replay-pipeline.json", QByteArray::fromStdString(pipeline.dump(2) + "\n"));
                report.insert("value_time", options.before ? "before_command" : "after_command");
                report.insert("known_fields", qint64(pipeline["known_fields"].get<uint64_t>()));
                report.insert("unknown_fields", qint64(pipeline["unknown_fields"].get<uint64_t>()));
            } else if ((command != "texture" && command != "buffer") || options.until)
                replay.run(progress);
            if (command == "geometry") {
                nlohmann::json geometry;
                replay.inspectEventInputs(options.until,
                                          [&] { geometry = inspectGeometry(frame, replay, options.until); });
                exportGeometry(geometry, out);
                report.insert("event", QString::number(options.until));
                report.insert("vertices", qint64(geometry["vertex_references"].get<uint64_t>()));
                report.insert("unique_vertices", qint64(geometry["unique_vertices"].get<uint64_t>()));
            } else if (command == "predicate") {
                auto detail = inspectPredicate(frame, replay, parseId("id"));
                detail["event"] = options.until;
                detail["value_time"] = options.before ? "before_command" : "after_command";
                save(out + "/predicate.json", QByteArray::fromStdString(detail.dump(2) + "\n"));
                report.insert("predicate",
                              QJsonDocument::fromJson(QByteArray::fromStdString(detail.dump())).object());
                report.insert("value_time", options.before ? "before_command" : "after_command");
            } else if (command == "buffer") {
                if (!p.isSet("id"))
                    throw std::runtime_error("buffer requires --id");
                auto resource = frame.resource(parseId("id"));
                if (resource.type != 0x83)
                    throw std::runtime_error("Not a buffer resource");
                if (!options.until && !resource.data)
                    throw std::runtime_error("Buffer has no captured initial bytes; select an event");
                std::vector<uint8_t> bytes;
                nlohmann::json counters;
                auto read = [&] {
                    bytes = replay.readBuffer(resource.id);
                    counters = inspectUavCounters(frame, replay, options.until, resource.id);
                };
                if (options.before && options.until && isDraw(frame.entry(options.until).type))
                    replay.inspectEventInputs(options.until, read);
                else
                    read();
                report.insert("uav_counters",
                              QJsonDocument::fromJson(QByteArray::fromStdString(counters.dump())).array());
                auto constants =
                    options.until && isDraw(frame.entry(options.until).type)
                        ? inspectConstants(frame, replay, options, options.until, resource.id, bytes)
                        : nlohmann::json::array();
                report.insert("constant_bindings",
                              QJsonDocument::fromJson(QByteArray::fromStdString(constants.dump())).array());
                constantBindings = std::move(constants);
                if (options.until && isDraw(frame.entry(options.until).type)) {
                    auto event = frame.event(options.until);
                    auto state =
                        effectiveSrvBindings(frame, event.id, frame.state(event.state), options.srvSetters);
                    auto bindings = bufferBindings(frame, event, state, resource.id);
                    QJsonArray list;
                    for (auto &binding : bindings) {
                        QJsonObject item{{"role", QString::fromStdString(binding.role)}};
                        if (!binding.stage.empty())
                            item.insert("stage", QString::fromStdString(binding.stage));
                        if (binding.slot >= 0)
                            item.insert("slot", binding.slot);
                        if (binding.view)
                            item.insert("view", QString::number(binding.view));
                        if (binding.role == "so")
                            item.insert("offset", qint64(binding.offset));
                        list.append(item);
                    }
                    report.insert("bindings", list);
                    report.insert("edit_effect", bindings.empty() ? QJsonValue(QJsonValue::Null)
                                                                  : QJsonValue(persistentBufferEdit(bindings)
                                                                                   ? "persistent_output"
                                                                                   : "scoped_input"));
                }
                auto offset = parseId("offset");
                if (offset > bytes.size())
                    throw std::runtime_error("Buffer offset exceeds resource size");
                auto length = p.isSet("length") ? parseId("length") : bytes.size() - offset;
                if (length > bytes.size() - offset)
                    throw std::runtime_error("Buffer range exceeds resource size");
                QByteArray raw(reinterpret_cast<const char *>(bytes.data() + offset), qsizetype(length));
                save(out + "/buffer.bin", raw);
                report.insert("sha256", digest(raw));
                report.insert("resource", QString::number(resource.id));
                report.insert("offset", qint64(offset));
                report.insert("length", qint64(length));
                report.insert("value_time", options.until ? (options.before ? "before_event" : "after_event")
                                                          : "capture_initial");
                QByteArray csv = "byte_offset,hex_bytes,uint32,int32,float32\n";
                for (qsizetype pos = 0; pos < raw.size(); pos += 4) {
                    auto word = raw.mid(pos, 4);
                    csv += QByteArray::number(offset + pos) + ',' + word.toHex();
                    if (word.size() == 4) {
                        uint32_t u;
                        int32_t i;
                        float f;
                        std::memcpy(&u, word.constData(), 4);
                        std::memcpy(&i, word.constData(), 4);
                        std::memcpy(&f, word.constData(), 4);
                        csv += ',' + QByteArray::number(u) + ',' + QByteArray::number(i) + ',' +
                               QByteArray::number(double(f), 'g', 17);
                    } else
                        csv += ",,,";
                    csv += '\n';
                }
                save(out + "/words.csv", csv);
            } else if (command != "replay-pipeline") {
                auto image =
                    command == "texture"
                        ? replay.previewTexture(parseId("id"), parseIndex("mip"), parseIndex("layer"),
                                                parseIndex("slice"))
                        : replay.output(p.isSet("id") ? parseId("id") : 0, parseIndex("subresource"));
                if (command == "texture")
                    report.insert("value_time", options.until
                                                    ? (options.before ? "before_event" : "after_event")
                                                    : "capture_initial");
                QByteArray raw(reinterpret_cast<const char *>(image.rgba.data()),
                               qsizetype(image.rgba.size()));
                save(out + "/frame.rgba", raw);
                QImage preview(image.rgba.data(), int(image.width), int(image.height), int(image.width * 4),
                               QImage::Format_RGBA8888);
                if (!preview.save(out + "/frame.png"))
                    throw std::runtime_error("Cannot write PNG");
                report.insert("width", int(image.width));
                report.insert("height", int(image.height));
                report.insert("resource", QString::number(image.resource));
                report.insert("rgba_sha256", digest(raw));
            }
            QJsonObject counts;
            for (auto &[key, value] : replay.counts)
                counts.insert(QString::fromStdString(key), qint64(value));
            report.insert("counts", counts);
            QJsonArray soHistory;
            for (const auto &row : replay.streamOutputHistory)
                soHistory.append(QJsonObject{{"event", QString::number(row.event)},
                                             {"stream", int(row.stream)},
                                             {"vertices_per_primitive", int(row.factor)},
                                             {"primitives_written", QString::number(row.written)},
                                             {"primitives_storage_needed", QString::number(row.needed)}});
            report.insert("stream_output_history", soHistory);
            if (auto it = replay.drawAutoResults().find(options.until);
                it != replay.drawAutoResults().end()) {
                const auto &parameters = it->second;
                auto detail = drawAutoJson(parameters);
                detail["parameters"] = {{"vertex_count", parameters.vertexCount}, {"start_vertex", 0}};
                report.insert("draw_auto",
                              QJsonDocument::fromJson(QByteArray::fromStdString(detail.dump())).object());
            }
            if (options.timings) {
                QJsonArray timings;
                for (auto &row : replay.timings)
                    timings.append(QJsonObject{{"event", QString::number(row.event)},
                                               {"microseconds", row.microseconds}});
                report.insert("timings", timings);
                auto &s = replay.statistics;
                report.insert("pipeline_statistics",
                              QJsonObject{{"IA Vertices", QString::number(s.IAVertices)},
                                          {"IA Primitives", QString::number(s.IAPrimitives)},
                                          {"VS Invocations", QString::number(s.VSInvocations)},
                                          {"GS Invocations", QString::number(s.GSInvocations)},
                                          {"GS Primitives", QString::number(s.GSPrimitives)},
                                          {"Clipper Invocations", QString::number(s.CInvocations)},
                                          {"Clipper Primitives", QString::number(s.CPrimitives)},
                                          {"PS Invocations", QString::number(s.PSInvocations)},
                                          {"HS Invocations", QString::number(s.HSInvocations)},
                                          {"DS Invocations", QString::number(s.DSInvocations)},
                                          {"CS Invocations", QString::number(s.CSInvocations)}});
            }

            report.insert("loaded_modules", modules());
            report.insert("completed", true);
        } else
            throw std::runtime_error("Unknown command");
        auto nativeReport = nlohmann::json::parse(QJsonDocument(report).toJson().toStdString());
        // QJson normalizes -0.0 to 0. Preserve reflected values and exact scalar bits in the report.
        if (!constantBindings.is_null())
            nativeReport["constant_bindings"] = std::move(constantBindings);
        auto json = QByteArray::fromStdString(nativeReport.dump(2) + "\n");
        if (!out.isEmpty())
            save(out + "/report.json", json);
        QTextStream(stdout) << json;
        return 0;
    } catch (const std::exception &e) {
        QJsonObject error{{"error", QString::fromUtf8(e.what())}, {"completed", false}};
        QTextStream(stderr) << QJsonDocument(error).toJson(QJsonDocument::Compact) << Qt::endl;
        return 1;
    }
}
