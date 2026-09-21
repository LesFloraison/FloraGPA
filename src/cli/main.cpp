#include "RdcAnalyzeCli.h"
#include "application/Annotations.h"
#include "application/ApiCommands.h"
#include "application/CaptureNames.h"
#include "application/CheckpointInspection.h"
#include "application/ClassInspector.h"
#include "application/CommandState.h"
#include "application/Constants.h"
#include "application/ContextInspector.h"
#include "application/Coverage.h"
#include "application/Experiment.h"
#include "application/ExperimentReport.h"
#include "application/ExternalShaderTools.h"
#include "application/FrameOutput.h"
#include "application/Geometry.h"
#include "application/GpuStatistics.h"
#include "application/GpuProfile.h"
#include "application/HlslCompilation.h"
#include "application/HlslRecovery.h"
#include "application/InvocationSelector.h"
#include "application/PlanarWrites.h"
#include "application/PostTransform.h"
#include "application/PredicateInspector.h"
#include "application/ReplayPipeline.h"
#include "application/ShaderInspector.h"
#include "application/ShaderProject.h"
#include "application/StreamOutputInspector.h"
#include "application/SystemDisassembly.h"
#include "application/TextureInspector.h"
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
#include <QFileInfo>
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
    if (app.arguments().size() > 1 && app.arguments()[1] == "rdc-analyze")
        return rdcAnalyzeCli(app.arguments().mid(1));
    QCommandLineParser p;
    p.setApplicationDescription("Native DX11 capture inspection and isolated replay");
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument(
        "command",
        "inventory | commands | command-state | contexts | command-lists | replay | shader | "
        "buffer | texture | texture-storage | compile | compile-project | assemble | geometry | replay-pipeline | "
        "class-linkage | predicate | annotations | statistics | timings | coverage | post-geometry | shader-checkpoint | "
        "rdc-analyze");
    p.addPositionalArgument("capture", "DX11 .gpa_frame file");
    p.addOption({"out", "New or empty output directory", "path"});
    p.addOption({"experiment", "Compatible FloraGPA experiment project", "path"});
    p.addOption({"source", "HLSL translation unit or shader project JSON", "path"});
    p.addOption({"entry", "HLSL entry point", "name", "main"});
    p.addOption({"profile", "Optional shader target profile for compile", "profile"});
    p.addOption({"optimization", "HLSL optimization: auto, preserve or optimize", "mode", "auto"});
    p.addOption({"decompiler", "Optional cmd_Decompiler.exe for recovery fallback or assembly", "path"});
    p.addOption({"recover", "Reconstruct supported DXBC as editable HLSL (shader only)"});
    p.addOption({"warp", "Use the WARP software adapter"});
    p.addOption({"timings", "Collect native GPU timestamps and pipeline statistics"});
    p.addOption({"debug-device", "Enable D3D11 validation"});
    p.addOption({"renderdoc", "Optional RenderDoc DLL for independent replay recapture", "path"});
    p.addOption({"event", "Stop at API event", "id"});
    p.addOption({"start-event", "Inclusive statistics range start", "id"});
    p.addOption({"end-event", "Inclusive statistics range end", "id"});
    p.addOption({"samples", "Repeated timing samples (1..1000)", "count", "5"});
    p.addOption({"warmup", "Timing warmup replays (0..1000)", "count", "1"});
    p.addOption({"include-writes", "Include resource-writing commands in repeated timings"});
    p.addOption({"coverage-mode", "Coverage mode: fragment or geometry", "mode", "fragment"});
    p.addOption({"coverage-target", "Coverage target: auto, depth or rt0..rt7", "target", "auto"});
    p.addOption({"coverage-layer", "Coverage array layer, volume slice or viewport array index", "index"});
    p.addOption({"ignore-depth", "Ignore depth/stencil tests for coverage"});
    p.addOption({"geometry-stage",
                 "Geometry stage: final, vs, hs, ds, gs, vs-index, vs-writes, ds-writes, gs-emits", "stage",
                 "final"});
    p.addOption({"stream", "Post-transform SO stream 0..3", "index", "0"});
    p.addOption({"instance", "Zero-based instance within the original draw", "index"});
    p.addOption({"shader-stage", "Checkpoint shader stage: gs, ds or hs", "stage", "gs"});
    p.addOption({"instruction", "Original shader instruction number for a checkpoint", "index"});
    p.addOption({"trace", "Capture the original invocation instruction trace"});
    p.addOption({"hs-phase", "Original HS phase ID", "index"});
    p.addOption({"input-selector", "Exact declared-input selector JSON", "path"});
    p.addOption({"max-bytes", "Checkpoint capture byte limit", "bytes", "268435456"});
    p.addOption({"before", "Stop before the selected event"});
    p.addOption({"id", "Resource ID", "id"});
    p.addOption({"filter", "API command text filter", "text"});
    p.addOption({"resource", "Filter API commands by referenced resource", "id"});
    p.addOption({"output-target", "Output target: auto, present, rt0..rt7, depth, stencil or swap:<ID>",
                 "target", "auto"});
    p.addOption({"output-channel", "Output channel: rgba, rgb, r, g, b or a", "channel", "rgba"});
    p.addOption({"output-low", "Output display range minimum", "value", "0"});
    p.addOption({"output-high", "Output display range maximum", "value", "1"});
    p.addOption({"output-layer", "Absolute output array layer or volume slice", "index"});
    p.addOption({"output-sample", "Output sample index", "index"});
    p.addOption({"subresource", "Texture subresource", "index", "0"});
    p.addOption({"mip", "Texture mip level", "index", "0"});
    p.addOption({"layer", "Texture array layer", "index", "0"});
    p.addOption({"slice", "Texture depth slice", "index", "0"});
    p.addOption({"sample", "Texture MSAA sample (default: resolve)", "index"});
    p.addOption({"typed-format", "Texture DXGI view format", "format"});
    p.addOption({"plane", "Texture plane: auto, y or uv", "plane", "auto"});
    p.addOption({"channel", "Texture display channel", "channel", "rgba"});
    p.addOption({"low", "Texture display minimum", "value", "0"});
    p.addOption({"high", "Texture display maximum", "value", "1"});
    p.addOption({"no-preview", "Export texture storage without a display conversion"});
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
        if (command != "coverage" && (p.isSet("coverage-mode") || p.isSet("coverage-target") ||
            p.isSet("coverage-layer") || p.isSet("ignore-depth")))
            throw std::runtime_error("Coverage options apply to coverage only");
        if (p.isSet("recover") && command != "shader")
            throw std::runtime_error("--recover applies to shader only");
        if ((p.isSet("profile") || p.value("optimization") != "auto") && command != "compile")
            throw std::runtime_error("--profile and --optimization apply to compile only");
        if (command != "timings" && (p.value("samples") != "5" || p.value("warmup") != "1" || p.isSet("include-writes")))
            throw std::runtime_error("--samples, --warmup and --include-writes apply to timings only");
        if (p.isSet("decompiler") && command != "shader" && command != "assemble")
            throw std::runtime_error("--decompiler applies to shader or assemble only");
        if (p.isSet("renderdoc") && command != "replay")
            throw std::runtime_error("--renderdoc applies to replay only");
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
        nlohmann::json constantBindings, outputSelection, outputDisplay, outputMsaa, planarWrites, experiment;
        nlohmann::json shaderProjectReport;
        QString out = p.value("out");
        if (!out.isEmpty()) {
            QDir dir(out);
            if (dir.exists() &&
                !dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden).empty())
                throw std::runtime_error("Output directory must be new or empty");
            if (!QDir().mkpath(out))
                throw std::runtime_error("Cannot create output directory");
        }
        if (command == "coverage") {
            if (out.isEmpty() || !p.isSet("id") || p.isSet("before") || p.isSet("timings") ||
                p.isSet("event") || p.isSet("suppress-draws"))
                throw std::runtime_error("Coverage requires --id and --out and defines its own replay boundary");
            ReplayOptions options;
            options.warp = p.isSet("warp");
            options.debug = p.isSet("debug-device");
            options.until = parseId("id");
            if (p.isSet("experiment")) {
                Experiment project(frame);
                project.load(p.value("experiment"), frame);
                project.apply(frame, options);
            }
            for (const auto &value : p.value("disable").split(',', Qt::SkipEmptyParts)) {
                bool valid = false;
                const auto id = value.toULongLong(&valid);
                if (!valid || !frame.entries().contains(id) || frame.entry(id).category != 7)
                    throw std::runtime_error("Invalid disabled event");
                options.disabled.insert(id);
            }
            CoverageOptions request;
            request.mode = p.value("coverage-mode").toStdString();
            request.target = p.value("coverage-target").toStdString();
            request.depthTest = !p.isSet("ignore-depth");
            if (p.isSet("coverage-layer")) request.layer = parseIndex("coverage-layer");
            Replay replay(frame, options);
            auto result = captureCoverage(replay, options.until, request);
            exportCoverage(result, std::filesystem::path(out.toStdWString()));
            shaderProjectReport = {{"coverage", result.report}};
            report.insert("loaded_modules", modules());
            report.insert("completed", true);
        } else if (command == "timings") {
            if (out.isEmpty() || p.isSet("before") || p.isSet("timings") || p.isSet("event") || p.isSet("suppress-draws"))
                throw std::runtime_error("Repeated timings require --out and define their own boundaries");
            if(p.isSet("id") && p.isSet("start-event"))
                throw std::runtime_error("Use --id or --start-event for the profile start");
            nlohmann::json values{{"samples",parseId("samples")},{"warmup",parseId("warmup")},{"include_writes",p.isSet("include-writes")}};
            if(p.isSet("id") || p.isSet("start-event")) values["start"]=parseId(p.isSet("id")?"id":"start-event");
            if(p.isSet("end-event")) values["end"]=parseId("end-event");
            const auto request=gpuProfileRequest(values);
            const auto selected=gpuProfileSelection(frame,request);
            ReplayOptions options;options.warp=p.isSet("warp");options.debug=p.isSet("debug-device");options.until=selected.end;
            if(p.isSet("experiment")){Experiment project(frame);project.load(p.value("experiment"),frame);project.apply(frame,options);}
            for(const auto &value:p.value("disable").split(',',Qt::SkipEmptyParts)) {
                bool valid=false;const auto id=value.toULongLong(&valid);
                if(!valid || !frame.entries().contains(id) || frame.entry(id).category!=7)throw std::runtime_error("Invalid disabled event");
                options.disabled.insert(id);
            }
            Replay replay(frame,options);
            shaderProjectReport=profileGpu(replay,frame,request,out);
            report.insert("loaded_modules",modules());
            report.insert("completed",true);
        } else if (command == "class-linkage") {
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
        } else if (command == "annotations") {
            if (out.isEmpty())
                throw std::runtime_error("annotations requires --out");
            const auto detail = inspectAnnotations(frame);
            exportAnnotations(detail, std::filesystem::path(out.toStdWString()));
            report.insert("completed", true);
            report.insert("annotation_records", qint64(detail.at("record_count").get<uint64_t>()));
            report.insert("loaded_modules", modules());
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
        } else if (command == "compile-project") {
            if (out.isEmpty() || !p.isSet("id") || !p.isSet("source"))
                throw std::runtime_error("compile-project requires --id, --source and --out");
            const auto id = parseId("id");
            Experiment shaderExperiment(frame);
            if (p.isSet("experiment"))
                shaderExperiment.load(p.value("experiment"), frame);
            const auto current = shaderExperiment.shaderBytes(frame, id);
            const auto original = inspectResourceShader(frame, id, current);
            QFile source(p.value("source"));
            if (!source.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read shader project");
            const auto project = validateShaderProject(nlohmann::json::parse(source.readAll().toStdString()));
            const auto compiled = compileShaderProject(project);
            auto metadata = inspectResourceShader(frame, id, compiled.bytecode);
            if (original.at("stage") != "signature" && metadata.at("stage") != original.at("stage"))
                throw std::runtime_error("Shader project stage does not match selected shader");
            save(out + "/replacement.dxbc",
                 QByteArray(reinterpret_cast<const char *>(compiled.bytecode.data()),
                            qsizetype(compiled.bytecode.size())));
            save(out + "/shader_project.json", QByteArray::fromStdString(project.dump(2, ' ', true)));
            auto assembly = QByteArray::fromStdString(systemDisassembly(compiled.bytecode, 0x80));
            assembly.replace("\r\n", "\n");
            assembly.replace("\n", "\r\n");
            save(out + "/replacement.asm", assembly);
            metadata["resource_id"] = id;
            metadata["compilation"] = compiled.report;
            shaderProjectReport = std::move(metadata);
            report.insert("completed", true);
        } else if (command == "assemble") {
            if (out.isEmpty() || !p.isSet("id") || !p.isSet("source") || !p.isSet("decompiler"))
                throw std::runtime_error("assemble requires --id, --source, --decompiler and --out");
            Experiment shaderExperiment(frame);
            if (p.isSet("experiment"))
                shaderExperiment.load(p.value("experiment"), frame);
            const auto original = shaderExperiment.shaderBytes(frame, parseId("id"));
            QFile source(p.value("source"));
            if (!source.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read DXBC assembly");
            shaderProjectReport = assembleShader(original, source.readAll(), out, p.value("decompiler"));
            report.insert("completed", true);
        } else if (command == "compile") {
            if (out.isEmpty() || !p.isSet("id") || !p.isSet("source"))
                throw std::runtime_error("compile requires --id, --source and --out");
            Experiment shaderExperiment(frame);
            if (p.isSet("experiment"))
                shaderExperiment.load(p.value("experiment"), frame);
            auto info = inspectResourceShader(frame, parseId("id"), shaderExperiment.shaderBytes(frame, parseId("id")));
            const auto profile = p.isSet("profile") ? p.value("profile").toStdString() :
                (info.at("profile").is_string() ? info.at("profile").get<std::string>() : std::string{});
            if (profile.empty())
                throw std::runtime_error("Output signature has no program to compile; specify --profile");
            QFile source(p.value("source"));
            if (!source.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot read HLSL source");
            auto bytes = source.readAll();
            HlslCompilation compiled;
            try {
                compiled = compileHlsl(bytes.toStdString(), profile, p.value("entry").toStdString(),
                    QDir::toNativeSeparators(QFileInfo(p.value("source")).absoluteFilePath()).toStdString(),
                    p.value("optimization").toStdString());
            } catch (const std::exception &e) {
                save(out + "/compiler.log", QByteArray(e.what()));
                throw;
            }
            save(out + "/compiler.log", QByteArray::fromStdString(compiled.diagnostics));
            auto metadata = inspectResourceShader(frame, parseId("id"), compiled.bytecode);
            if (info.at("stage") != "signature" && metadata.at("stage") != info.at("stage"))
                throw std::runtime_error("Replacement shader stage does not match the selected resource");
            save(out + "/replacement.dxbc",
                 QByteArray(reinterpret_cast<const char *>(compiled.bytecode.data()),
                            qsizetype(compiled.bytecode.size())));
            report.insert("completed", true);
            report.insert("profile", QString::fromStdString(profile));
            save(out + "/replacement.hlsl", bytes);
            auto assembly = QByteArray::fromStdString(systemDisassembly(compiled.bytecode, 0x80));
            assembly.replace("\r\n", "\n"); assembly.replace("\n", "\r\n");
            save(out + "/replacement.asm", assembly);
            metadata.update({{"compilation", compiled.options}, {"diagnostics", compiled.diagnostics}, {"resource_id", parseId("id")}});
            shaderProjectReport = std::move(metadata);
        } else if (command == "shader") {
            if (!p.isSet("id") || out.isEmpty())
                throw std::runtime_error("shader requires --id and --out");
            Experiment shaderExperiment(frame);
            if (p.isSet("experiment"))
                shaderExperiment.load(p.value("experiment"), frame);
            auto bytes = shaderExperiment.shaderBytes(frame, parseId("id"));
            save(out + "/shader.dxbc",
                 QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())));
            auto metadata = inspectResourceShader(frame, parseId("id"), bytes);
            auto assembly = QByteArray::fromStdString(metadata.at("stage") == "signature" ? disassemble(bytes) : systemDisassembly(bytes, 0x80));
            assembly.replace("\r\n", "\n"); assembly.replace("\n", "\r\n");
            save(out + "/shader.asm", assembly);
            const auto project = shaderExperiment.shaderProject(parseId("id"));
            if (!project.is_null()) {
                try {
                    auto source = verifyShaderProject(bytes, project);
                    source["available"] = true;
                    metadata["source_project"] = source;
                    save(out + "/shader_project.json", QByteArray::fromStdString(project.dump(2, ' ', true)));
                } catch (const std::exception &e) {
                    metadata["source_project"] = {{"available", false}, {"error", e.what()}};
                }
            }
            if (p.isSet("recover") || p.isSet("decompiler")) {
                if (metadata.at("stage") == "signature")
                    metadata["decompilation"] = {
                        {"available", false},
                        {"reason", "Output signature only; no executable shader program or HLSL body"}};
                else {
                    metadata["decompilation"] = exportRecoveredHlsl(bytes, out, p.value("decompiler"),
                        shaderExperiment.shaderSource(parseId("id")));
                }
            }
            shaderProjectReport = metadata;
            save(out + "/shader.json", QByteArray::fromStdString(metadata.dump(2)));
            save(out + "/shader_sources.json",
                 QByteArray::fromStdString(metadata.at("embedded_sources").dump(2, ' ', true)));
            const auto &sources = metadata["embedded_sources"]["files"];
            if (!sources.empty()) {
                QDir().mkpath(out + "/shader_sources");
                for (size_t i = 0; i < sources.size(); ++i)
                    save(out + QString("/shader_sources/%1.hlsl").arg(i, 4, 10, QChar('0')),
                         QByteArray::fromHex(
                             QByteArray::fromStdString(sources[i]["raw_hex"].get<std::string>())));
            }
            report.insert("id", p.value("id"));
            report.insert("completed", true);
        } else if (command == "replay" || command == "buffer" || command == "texture" ||
                   command == "statistics" || command == "texture-storage" || command == "geometry" ||
                   command == "post-geometry" || command == "replay-pipeline" || command == "predicate" ||
                   command == "shader-checkpoint") {
            if (out.isEmpty())
                throw std::runtime_error("Replay requires --out");
            ReplayOptions options;
            options.timings = p.isSet("timings");
            options.warp = p.isSet("warp");
            options.debug = p.isSet("debug-device");
            if (p.isSet("renderdoc")) {
                options.renderdocLibrary = p.value("renderdoc").toStdWString();
                options.renderdocOutput = QDir(out).absoluteFilePath("independent").toStdWString();
            }
            options.suppressDraws = p.isSet("suppress-draws");
            options.before = p.isSet("before");
            options.prepareBeforeDraw = command != "replay";
            if (p.isSet("event"))
                options.until = parseId("event");
            if (command == "statistics") {
                if (p.isSet("before") || p.isSet("timings"))
                    throw std::runtime_error("Statistics defines its own query boundaries");
                const bool range = p.isSet("start-event") || p.isSet("end-event");
                if (range && (p.isSet("event") || !p.isSet("start-event") || !p.isSet("end-event")))
                    throw std::runtime_error("Specify a single event or both range endpoints");
                if (!range && !p.isSet("event"))
                    throw std::runtime_error("Statistics requires an event or command range");
                const auto start = range ? parseId("start-event") : options.until;
                const auto end = range ? parseId("end-event") : options.until;
                options.until = end;
                options.measurement = ReplayOptions::Measurement{start, end, !range};
            }
            if (command == "predicate" && (!options.until || !p.isSet("id")))
                throw std::runtime_error("predicate requires --event and --id");
            if (command == "geometry" || command == "post-geometry" || command == "shader-checkpoint") {
                if (!options.until)
                    throw std::runtime_error(command.toStdString() + " requires --event");
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
            } else if ((command != "texture" && command != "buffer" && command != "texture-storage") ||
                       options.until)
                replay.run(progress);
            if (command == "statistics") {
                const auto statistics = gpuStatisticsReport(replay);
                exportGpuStatistics(statistics, std::filesystem::path(out.toStdWString()));
            } else if (command == "post-geometry") {
                PostTransformOptions inspect;
                inspect.stage = p.value("geometry-stage").toStdString();
                bool valid = false;
                inspect.stream = p.value("stream").toUInt(&valid);
                if (!valid || inspect.stream > 3)
                    throw std::runtime_error("SO stream must be 0..3");
                if (p.isSet("instance")) {
                    inspect.instance = p.value("instance").toUInt(&valid);
                    if (!valid)
                        throw std::runtime_error("Instance must be an unsigned integer");
                }
                exportPostTransform(inspectPostTransform(replay, options.until, inspect), out.toStdWString());
            } else if (command == "geometry") {
                nlohmann::json geometry;
                replay.inspectEventInputs(options.until,
                                          [&] { geometry = inspectGeometry(frame, replay, options.until); });
                exportGeometry(geometry, out);
                report.insert("event", QString::number(options.until));
                report.insert("vertices", qint64(geometry["vertex_references"].get<uint64_t>()));
                report.insert("unique_vertices", qint64(geometry["unique_vertices"].get<uint64_t>()));
            } else if (command == "shader-checkpoint") {
                if (p.isSet("instance") || p.isSet("stream"))
                    throw std::runtime_error(
                        "Shader checkpoints inspect all instances and do not select an output stream");
                CheckpointInspectionOptions request;
                request.stage = p.value("shader-stage").toStdString();
                request.trace = p.isSet("trace");
                request.maxBytes = parseId("max-bytes");
                if (p.isSet("instruction"))
                    request.instruction = parseIndex("instruction");
                if (p.isSet("hs-phase"))
                    request.hullPhase = parseIndex("hs-phase");
                if (p.isSet("input-selector"))
                    request.inputSelector = checkpoint::readSelector(
                        std::filesystem::path(p.value("input-selector").toStdWString()));
                const auto inspection = inspectCheckpoint(replay, options.until, request);
                exportCheckpoint(inspection, std::filesystem::path(out.toStdWString()));
                report.insert("checkpoint_file", "checkpoint.json");
                report.insert("record_count", qint64(inspection.report.at("record_count").get<uint64_t>()));
                report.insert("shader_stage", QString::fromStdString(request.stage));
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
                    auto state = effectiveBindings(frame, event.id, frame.state(event.state), options);
                    auto bindings = bufferBindings(replay.frame(), event, state, resource.id);
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
            } else if (command == "texture-storage") {
                const auto id = parseId("id");
                std::vector<uint8_t> bytes;
                if (!options.until && !frame.resource(id).data)
                    throw std::runtime_error("Texture has no captured initial bytes; select an event");
                auto read = [&] { bytes = replay.readTexture(id); };
                if (options.before && options.until && isDraw(frame.entry(options.until).type))
                    replay.inspectEventInputs(options.until, read);
                else
                    read();
                QByteArray raw(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
                save(out + "/texture.bin", raw);
                report.insert("resource", QString::number(id));
                report.insert("sha256", digest(raw));
                report.insert("length", qint64(bytes.size()));
                report.insert("value_time", options.until ? (options.before ? "before_event" : "after_event")
                                                          : "capture_initial");
            } else if (command != "replay-pipeline") {
                Image image;
                bool available = true;
                if (command == "texture") {
                    TextureInspectionOptions selection;
                    selection.mip = parseIndex("mip");
                    selection.layer = parseIndex("layer");
                    selection.slice = parseIndex("slice");
                    selection.channel = p.value("channel").toStdString();
                    selection.plane = p.value("plane").toStdString();
                    if (p.isSet("sample"))
                        selection.sample = parseIndex("sample");
                    if (p.isSet("typed-format"))
                        selection.typedFormat = parseIndex("typed-format");
                    bool lowOk = false, highOk = false;
                    selection.low = p.value("low").toDouble(&lowOk);
                    selection.high = p.value("high").toDouble(&highOk);
                    if (!lowOk || !highOk)
                        throw std::runtime_error("Invalid texture display range");
                    selection.preview = !p.isSet("no-preview");
                    auto inspected = inspectTexture(replay, parseId("id"), selection);
                    report.insert("reference_pixels_used", frame.resource(parseId("id")).type == 0x87);
                    exportTextureInspection(inspected, std::filesystem::path(out.toStdWString()));
                    available = inspected.image.has_value();
                    if (available)
                        image = std::move(*inspected.image);
                    report.insert("texture", QJsonDocument::fromJson(
                                                 QByteArray::fromStdString(inspected.metadata.dump()))
                                                 .object());
                    report.insert("image_available", available);
                    report.insert("value_time", options.until
                                                    ? (options.before ? "before_event" : "after_event")
                                                    : "capture_initial");
                } else {
                    FrameDisplayOptions display;
                    display.channel = p.value("output-channel").toStdString();
                    bool lowOk = false, highOk = false;
                    display.low = p.value("output-low").toDouble(&lowOk);
                    display.high = p.value("output-high").toDouble(&highOk);
                    if (!lowOk || !highOk)
                        throw std::runtime_error("Invalid output display range");
                    displayOutput({}, 28, display);
                    if (p.isSet("output-layer"))
                        display.layer = parseIndex("output-layer");
                    if (p.isSet("output-sample"))
                        display.sample = parseIndex("output-sample");
                    outputSelection = selectFrameOutput(replay, p.value("output-target").toStdString());
                    if (p.isSet("id")) {
                        if (p.isSet("output-target"))
                            throw std::runtime_error("Choose a resource override or an output target");
                        const auto id = parseId("id");
                        const auto info = textureInfo(frame.resource(id));
                        const auto sub = parseIndex("subresource");
                        if (!info.mips || sub >= uint64_t(info.mips) * info.layers)
                            throw std::runtime_error("Output subresource exceeds storage");
                        display.mip = sub % info.mips;
                        if (!display.layer)
                            display.layer = sub / info.mips;
                        outputSelection["resource"] = id;
                        outputSelection["view"] = 0;
                        outputSelection["kind"] = "explicit_resource";
                    }
                    available = !outputSelection["resource"].is_null();
                    if (available) {
                        auto result = readFrameOutput(replay, outputSelection["resource"].get<Id>(), display,
                                                      outputSelection["view"].is_null()
                                                          ? std::optional<Id>{}
                                                          : outputSelection["view"].get<Id>(),
                                                      outputSelection.value("aspect", std::string{}));
                        image = std::move(result.image);
                        outputDisplay = std::move(result.display);
                        outputMsaa = std::move(result.msaa);
                        save(out + "/output_storage.bin",
                             QByteArray(reinterpret_cast<const char *>(result.storage.data()),
                                        qsizetype(result.storage.size())));
                        save(out + "/output_storage.json",
                             QByteArray::fromStdString(outputDisplay.dump(2) + "\n"));
                    }
                    report.insert("image_available", available);
                    report.insert("image_status",
                                  QString::fromStdString(outputSelection["kind"].get<std::string>()));
                }
                if (available) {
                    QByteArray raw(reinterpret_cast<const char *>(image.rgba.data()),
                                   qsizetype(image.rgba.size()));
                    save(out + "/frame.rgba", raw);
                    QImage preview(image.rgba.data(), int(image.width), int(image.height),
                                   int(image.width * 4), QImage::Format_RGBA8888);
                    if (!preview.save(out + "/frame.png"))
                        throw std::runtime_error("Cannot write PNG");
                    report.insert("width", int(image.width));
                    report.insert("height", int(image.height));
                    report.insert("resource", QString::number(image.resource));
                    report.insert("rgba_sha256", digest(raw));
                } else {
                    for (auto key : {"width", "height", "resource", "rgba_sha256"})
                        report.insert(key, QJsonValue(QJsonValue::Null));
                }
            }
            QJsonObject counts;
            if (const auto capture = replay.finishCapture())
                report.insert("rdc_capture", QString::fromStdWString(capture->wstring()));
            for (auto &[key, value] : replay.counts)
                counts.insert(QString::fromStdString(key), qint64(value));
            report.insert("counts", counts);
            planarWrites = planarWriteReport(replay);
            experiment = experimentReport(replay);
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
        if (command == "compile-project" || command == "shader" || command == "compile" || command == "assemble")
            report.insert("loaded_modules", modules());
        auto nativeReport = nlohmann::json::parse(QJsonDocument(report).toJson().toStdString());
        if (!shaderProjectReport.is_null())
            nativeReport.update(shaderProjectReport);
        if (!outputSelection.is_null()) {
            nativeReport["output_selection"] = std::move(outputSelection);
            nativeReport["output_display"] = std::move(outputDisplay);
            nativeReport["output_msaa"] = std::move(outputMsaa);
        }
        // QJson normalizes -0.0 to 0. Preserve reflected values and exact scalar bits in the report.
        if (!constantBindings.is_null())
            nativeReport["constant_bindings"] = std::move(constantBindings);
        if (!planarWrites.is_null())
            nativeReport["planar_writes"] = std::move(planarWrites);
        nativeReport["experiment"] = std::move(experiment);
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
