#include "Dx12Cli.h"
#include "application/ReplaySession.h"
#include "application/Experiment.h"
#include "application/SystemDisassembly.h"
#include "application/DxilDisassembly.h"
#include "dx12/NativeDevice.h"
#include "dx12/InitializationCopy.h"
#include "dx12/PipelineRecipes.h"
#include "dx12/NativeReplay.h"
#include "replay/RenderDocCapture.h"
#include <QDir>
#include <QSaveFile>
#include <QTextStream>
#include <QImage>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <set>
#include <memory>

namespace flora {
namespace {
int exportDx12Shader(const IReplaySession &session, const QCommandLineParser &options, const QDir &output) {
    const auto idText = options.value("id");
    bool valid{};
    const auto creation = idText.toULongLong(&valid);
    if (!options.isSet("id") || !valid || !creation ||
        std::any_of(idText.begin(), idText.end(), [](QChar c) { return c < '0' || c > '9'; }))
        throw std::runtime_error("DX12 shader requires --id <pipeline creation record>");
    const auto stage = options.value("pipeline-stage").toUpper().toStdString();
    if (stage.empty()) throw std::runtime_error("DX12 shader requires --pipeline-stage <VS/PS/CS/HS/DS/GS/AS/MS>");
    dx12::StreamDirectory directory(session.path());
    if (creation > directory.packets().size()) throw std::runtime_error("Pipeline creation record is out of range");
    const auto &packet = directory.packets()[size_t(creation - 1)];
    if (!dx12::isPipelineRecipe(packet.type)) throw std::runtime_error("Selected record is not a pipeline creation");
    const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
    const auto found = std::find_if(recipe.shaders.begin(), recipe.shaders.end(),
        [&](const auto &shader) { return shader.first == stage; });
    if (found == recipe.shaders.end()) throw std::runtime_error("Selected pipeline has no shader for stage " + stage);
    const auto original = dx12::describeShaderBytecode(found->second);
    if (original.at("stage") != stage) throw std::runtime_error("Captured shader stage does not match its pipeline slot");
    std::vector<uint8_t> bytecode(found->second.begin(), found->second.end());
    nlohmann::json experimentInfo = nullptr;
    bool replaced = false;
    if (options.isSet("experiment")) {
        Experiment experiment(session);
        experiment.load(options.value("experiment"), session);
        for (const auto &edit : experiment.dx12Edits(session).shaders) {
            if (edit.pipeline != creation || edit.stage != stage) continue;
            if (edit.originalSha256 != original.at("sha256").get<std::string>())
                throw std::runtime_error("Original pipeline shader digest mismatch");
            bytecode = edit.bytecode;
            replaced = true;
        }
        const auto document = QByteArray::fromStdString(experiment.document().dump());
        experimentInfo = {{"cursor", std::to_string(experiment.revision())},
            {"document_sha256", QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex().toStdString()}};
    }
    auto report = dx12::describeShaderBytecode(bytecode);
    if (report.at("stage") != stage) throw std::runtime_error("Replacement shader stage mismatch");
    const auto filename = report.at("format") == "DXIL" ? "shader.dxil" : "shader.dxbc";
    report.update({{"backend", "dx12"}, {"completed", true}, {"status", "bytecode_exported"},
        {"capture_identity", session.captureIdentity()}, {"pipeline_creation", std::to_string(creation)},
        {"pipeline_object", std::to_string(recipe.object)}, {"root_signature_object", std::to_string(recipe.rootSignature)},
        {"original", original}, {"experiment", experimentInfo}, {"replaced", replaced}, {"file", filename},
        {"gpu_commands_executed", false},
        {"scope", "Selected captured PSO stage; export does not validate root-signature compatibility or GPU execution"},
        {"disassembly", {{"available", false}}}});
    const auto save = [&](const QString &name, const QByteArray &bytes) {
        QSaveFile file(output.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save DX12 shader export");
    };
    try {
        if (report.at("format") == "DXBC") {
            const auto assembly = systemDisassembly(bytecode, 0x80);
            save("shader.asm", QByteArray::fromStdString(assembly));
            report["disassembly"] = {{"available", true}, {"file", "shader.asm"}};
        } else {
            const auto library = options.isSet("dxcompiler") ? options.value("dxcompiler") :
                QDir(QCoreApplication::applicationDirPath()).filePath("DXCompiler/dxcompiler.dll");
            const auto assembly = disassembleDxil(bytecode, std::filesystem::path(library.toStdWString()));
            save("shader.asm", QByteArray::fromStdString(assembly.text));
            report["disassembly"] = {{"available", true}, {"file", "shader.asm"}, {"backend", assembly.backend}};
        }
    } catch (const std::exception &error) {
        report["disassembly"] = {{"available", false}, {"reason", error.what()}};
    }
    save(filename, QByteArray(reinterpret_cast<const char *>(bytecode.data()), qsizetype(bytecode.size())));
    const auto json = QByteArray::fromStdString(report.dump(2) + "\n");
    save("shader.json", json);
    save("report.json", json);
    QTextStream(stdout) << json << Qt::endl;
    return 0;
}
int replayDx12(const IReplaySession &session, const QString &path, const QCommandLineParser &options,
               const QDir &output, bool buffer = false) {
    const auto number = [&](const char *key) {
        bool valid{};
        const auto value = options.value(key).toULongLong(&valid);
        if (!valid || options.value(key).startsWith('-'))
            throw std::runtime_error(std::string("Invalid DX12 unsigned argument: --") + key);
        return uint64_t(value);
    };
    if (!options.isSet("event"))
        throw std::runtime_error(
            "DX12 replay requires --event <execution instance>; full-frame replay is unfinished");
    const auto execution = number("event"), budget = number("max-bytes");
    if (!execution || !budget)
        throw std::runtime_error("DX12 execution and allocation budget must be nonzero");
    const auto sub = number("subresource");
    if (sub > UINT_MAX)
        throw std::runtime_error("DX12 subresource exceeds uint32");
    uint64_t object{}, creation{};
    if (options.isSet("id")) {
        creation = number("id");
        object = session.resource(creation).capturedObject;
    } else if (options.isSet("subresource"))
        throw std::runtime_error("DX12 subresource requires --id <resource creation record>");
    uint64_t offset{}, length{};
    if (buffer) {
        if (!creation) throw std::runtime_error("DX12 buffer requires --id <resource creation record>");
        const auto desc = nlohmann::json::parse(session.resource(creation).summary);
        if (desc.at("Dimension") != 1) throw std::runtime_error("Selected DX12 resource is not a buffer");
        const auto width = std::stoull(desc.at("Width").get<std::string>());
        offset = number("offset");
        if (offset > width) throw std::runtime_error("Buffer offset exceeds resource size");
        length = options.isSet("length") ? number("length") : width - offset;
        if (!length || length > width - offset || length > 256ULL * 1024 * 1024)
            throw std::runtime_error("DX12 buffer range must fit the resource and be 1 byte to 256 MiB");
    }
    ReplaySelection selection;
    selection.session = session.identity();
    selection.execution = execution;
    selection.before = options.isSet("before");
    selection.resource = buffer ? 0 : creation;
    const auto capability = session.capability(ReplayFeature::Replay, selection);
    const bool isolateQueries = options.isSet("isolate-missing-queries");
    if (isolateQueries)
        for (const auto flag : {"renderdoc", "measure-event", "experiment", "disable", "uav-barrier-before"})
            if (options.isSet(flag)) throw std::runtime_error(std::string("Query isolation cannot be combined with --") + flag);
    bool isolatedIssue = false;
    if (isolateQueries && capability.status == CapabilityStatus::MissingData) {
        const auto submissions = session.submissionDetails();
        for (const auto &entry : submissions.at("query_replay_coverage").at("resolves_and_issues")) {
            if (entry.at("status") == "missing_query_results" && entry.at("reason") == capability.reason) {
                isolatedIssue = true; break;
            }
        }
    }
    if (!capability.supported() && !isolatedIssue) {
        const auto status = capability.status == CapabilityStatus::MissingData ? "missing_data" :
                            capability.status == CapabilityStatus::NotImplemented ? "not_implemented" :
                            capability.status == CapabilityStatus::UnsupportedDevice ? "unsupported_device" : "failed";
        const nlohmann::json report{{"backend", "dx12"}, {"completed", false}, {"frame_complete", false},
            {"image_available", false}, {"device_created", false}, {"replay_scope", "selected_execution"},
            {"execution", std::to_string(execution)}, {"event", std::to_string(execution)},
            {"value_time", selection.before ? "before_event" : "after_event"},
            {"capability_status", status}, {"error", capability.reason},
            {"query_replay_coverage", session.submissionDetails().at("query_replay_coverage")}};
        QSaveFile file(output.filePath("report.json"));
        const auto bytes = QByteArray::fromStdString(report.dump(2));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save DX12 replay capability report");
        QTextStream(stderr) << QString::fromStdString(capability.reason) << '\n';
        return 2;
    }
    std::optional<Experiment> experiment;
    if (options.isSet("experiment") || options.isSet("disable") || options.isSet("uav-barrier-before")) {
        experiment.emplace(session);
        if (options.isSet("experiment")) experiment->load(options.value("experiment"), session);
        if (options.isSet("disable")) for (const auto &id : options.value("disable").split(',')) {
            bool valid{};
            const auto editedExecution = id.toULongLong(&valid);
            if (!valid || !editedExecution || id != QString::number(editedExecution))
                throw std::runtime_error("DX12 --disable requires comma-separated decimal execution IDs");
            experiment->setEnabled(session, editedExecution, false);
        }
        if (options.isSet("uav-barrier-before")) for (const auto &id : options.value("uav-barrier-before").split(',')) {
            bool valid{};
            const auto editedExecution = id.toULongLong(&valid);
            if (!valid || !editedExecution || id != QString::number(editedExecution))
                throw std::runtime_error("DX12 --uav-barrier-before requires comma-separated decimal execution IDs");
            experiment->setUavBarrierBefore(session, editedExecution, true);
        }
        experiment->save(output.filePath("experiment.json"));
    }
    dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
    std::optional<RenderDocCapture> capture;
    // Keep the capture hook alive until the replay and device are destroyed,
    // including device/replay construction failures.
    std::unique_ptr<dx12::NativeDevice> deviceOwner;
    std::unique_ptr<dx12::NativeReplay> replayOwner;
    nlohmann::json report{{"backend", "dx12"},
                          {"completed", false},
                          {"frame_complete", false},
                          {"missing_query_policy", isolateQueries ? "isolate_readback_outputs" : "fail"},
                          {"replay_scope", "selected_execution"},
                          {"execution", std::to_string(execution)},
                          {"event", std::to_string(execution)},
                          {"value_time", options.isSet("before") ? "before_event" : "after_event"},
                          {"image_available", false},
                          {"width", nullptr},
                          {"height", nullptr},
                          {"resource", nullptr},
                          {"requested_resource", creation ? nlohmann::json(std::to_string(creation)) : nlohmann::json()},
                          {"allocation_limit", std::to_string(budget)},
                          {"rgba_sha256", nullptr}};
    if (experiment) {
        const auto document = QByteArray::fromStdString(experiment->document().dump());
        report["experiment"] = {{"backend", "dx12"}, {"capture_identity", experiment->document().at("capture_identity")},
            {"cursor", std::to_string(experiment->revision())},
            {"document_sha256", QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex().toStdString()}};
    }
    std::string stage = "load_analysis_library";
    try {
        if (options.isSet("renderdoc"))
            capture.emplace(std::filesystem::path(options.value("renderdoc").toStdWString()),
                            std::filesystem::path(output.filePath("native-dx12").toStdWString()), false);
        stage = "create_device";
        deviceOwner = std::make_unique<dx12::NativeDevice>(directory, options.isSet("debug-device"));
        auto &device = *deviceOwner;
        stage = "create_replay";
        replayOwner = std::make_unique<dx12::NativeReplay>(device, directory, budget);
        auto &replay = *replayOwner;
        replay.setIsolateMissingQueries(isolateQueries);
        stage = "apply_experiment";
        if (experiment) {
            auto edits = experiment->dx12Edits(session);
            replay.setDisabledExecutions(std::move(edits.disabled));
            replay.setUavBarriersBefore(std::move(edits.uavBarriersBefore));
            replay.setShaderReplacements(std::move(edits.shaders));
            replay.setResourcePatches(std::move(edits.resources));
        }
        stage = "prepare_execution";
        replay.prepareExecution(execution, !options.isSet("before"), bool(capture), options.isSet("measure-event"));
        stage = "initialize_transfers";
        replay.initializeTransfers();
        stage = "start_analysis_capture";
        if (capture)
            capture->start(device.device(), "FloraGPA independent DX12 replay");
        stage = "replay_execution";
        replay.replayTo(execution, !options.isSet("before"), bool(capture));
        if (options.isSet("measure-event"))
            report["performance"] = replay.performanceResult();
        stage = "finish_analysis_capture";
        if (capture)
            report["rdc_capture"] = QString::fromStdWString(capture->finish().wstring()).toStdString();
        stage = buffer ? "buffer_readback" : "texture_readback";
        if (buffer) {
            const auto bytes = replay.inspectBuffer(creation, offset, length,
                std::filesystem::path(output.filePath("buffer.bin").toStdWString()));
            report.update({{"resource", std::to_string(creation)}, {"offset", std::to_string(offset)},
                {"length", std::to_string(length)}, {"sha256", bytes.at("sha256")}, {"native_buffer", bytes}});
        } else if (object) {
            const auto texture = replay.readTexture(
                object, UINT(sub),
                std::filesystem::path(output.filePath("output_storage.bin").toStdWString()), creation);
            report["native_texture"] = texture;
            auto storage = texture;
            storage["backend"] = "dx12";
            storage["storage_bytes"] = std::stoull(texture.at("bytes").get<std::string>());
            storage["storage_sha256"] = texture.at("sha256");
            report["output_display"] = storage;
            QSaveFile metadata(output.filePath("output_storage.json"));
            const auto serialized = QByteArray::fromStdString(storage.dump(2) + "\n");
            if (!metadata.open(QIODevice::WriteOnly) || metadata.write(serialized) != serialized.size() ||
                !metadata.commit())
                throw std::runtime_error("Cannot save DX12 raw storage metadata");
            if (!texture.at("image").get<std::string>().empty()) {
                auto image =
                    QImage(output.filePath(QString::fromStdString(texture.at("image").get<std::string>())))
                        .convertToFormat(QImage::Format_RGBA8888);
                if (image.isNull())
                    throw std::runtime_error("Cannot decode native DX12 display image");
                QSaveFile raw(output.filePath("frame.rgba")), png(output.filePath("frame.png"));
                if (!raw.open(QIODevice::WriteOnly) || !png.open(QIODevice::WriteOnly))
                    throw std::runtime_error("Cannot open DX12 display output");
                QCryptographicHash hash(QCryptographicHash::Sha256);
                for (int y = 0; y < image.height(); ++y) {
                    const auto data = reinterpret_cast<const char *>(image.constScanLine(y));
                    const auto size = qint64(image.width()) * 4;
                    hash.addData(QByteArrayView(data, size));
                    if (raw.write(data, size) != size)
                        throw std::runtime_error("Cannot write DX12 display pixels");
                }
                if (!raw.commit() || !image.save(&png, "PNG") || !png.commit())
                    throw std::runtime_error("Cannot publish DX12 display output");
                report.update({{"image_available", true},
                               {"width", image.width()},
                               {"height", image.height()},
                               {"resource", std::to_string(creation)},
                               {"rgba_sha256", hash.result().toHex().toStdString()}});
            }
        }
        report["completed"] = true;
        report["image_status"] = buffer ? "Native GPU buffer range readback" : object ? (report["image_available"] == true
                                               ? "Native GPU readback; selected execution only"
                                               : "Raw GPU texture saved; this display format is unsupported")
                                        : "Execution replayed; select a texture creation record for readback";
        if (report["image_available"] == true && report.at("native_texture").at("format") == 26)
            report["image_status"] = "Native GPU readback; R11G11B10 preview uses linear RGB clamped to [0,1], alpha 1; raw HDR storage preserved";
        if (isolateQueries) report["image_status"] = report.at("image_status").get<std::string>() +
            "; incomplete query outputs isolated; historical values and full-frame fidelity are not restored";
    } catch (const std::exception &error) {
        if (capture)
            capture->discard();
        report["error"] = error.what();
        report["failure_stage"] = stage;
        report["completed"] = false;
        report["image_available"] = false;
        report["image_status"] = "Replay failed; any partial artifacts are not a completed result";
    }
    report["device"] = deviceOwner ? deviceOwner->information() : nlohmann::json();
    report["native_replay"] = replayOwner ? replayOwner->information() : nlohmann::json();
    QSaveFile file(output.filePath("report.json"));
    const auto bytes = QByteArray::fromStdString(report.dump(2));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save DX12 replay report");
    QTextStream(stdout) << QString::fromUtf8(bytes) << Qt::endl;
    if (!report.at("completed").get<bool>()) {
        const nlohmann::json error{{"completed", false}, {"error", report.at("error")},
                                   {"failure_stage", report.at("failure_stage")}};
        QTextStream(stderr) << QString::fromStdString(error.dump()) << Qt::endl;
        return 1;
    }
    return 0;
}
} // namespace
int runDx12Cli(const QString &command, const QString &path, const QCommandLineParser &options) {
    if (command != "inventory" && command != "commands" && command != "validate-frame" &&
        command != "command-lists" && command != "replay" && command != "buffer" && command != "shader")
        throw std::runtime_error("DX12 " + command.toStdString() +
                                 " is not implemented; use inventory or commands for stream inspection");
    std::set<QString> allowed{"out", "replay-identity"};
    if (command == "commands")
        allowed.insert("filter");
    if (command == "shader") allowed.insert({"id", "pipeline-stage", "experiment", "dxcompiler"});
    if (command == "replay")
        allowed.insert({"event", "before", "id", "subresource", "max-bytes", "renderdoc", "debug-device", "measure-event", "experiment", "disable"});
    if (command == "buffer")
        allowed.insert({"event", "before", "id", "offset", "length", "max-bytes", "debug-device", "experiment", "disable", "no-buffer-csv"});
    if (command == "replay" || command == "buffer") allowed.insert({"uav-barrier-before", "isolate-missing-queries"});
    if (command == "validate-frame") {
        allowed.insert({"check-device", "check-copy", "check-pipeline", "check-initialization"});
        if (options.isSet("check-copy") || options.isSet("check-initialization")) allowed.insert("max-bytes");
        if (options.isSet("check-initialization")) {
            allowed.insert({"id", "offset", "length", "verify-texture-copy", "check-execution", "texture-object", "subresource", "before", "debug-device"});
            if (options.isSet("check-execution")) allowed.insert("renderdoc");
        }
    }
    for (const auto &option : options.optionNames())
        if (!allowed.contains(option))
            throw std::runtime_error("Unsupported DX12 inspection option: --" + option.toStdString());
    if (options.value("out").isEmpty()) throw std::runtime_error("DX12 inspection requires --out");
    if (int(options.isSet("check-copy")) + int(options.isSet("check-pipeline")) + int(options.isSet("check-initialization")) > 1)
        throw std::runtime_error("Choose one targeted initialization check per invocation");
    const QDir output(options.value("out"));
    if (output.exists() && !output.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).empty())
        throw std::runtime_error("Output directory must be new or empty");
    auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
    if (!QDir().mkpath(output.path())) throw std::runtime_error("Cannot create DX12 inspection output directory");
    if (command == "shader") return exportDx12Shader(*session, options, output);
    if (command == "replay" || command == "buffer") return replayDx12(*session,path,options,output,command == "buffer");
    const auto filename = command == "commands" ? "commands.json" : command == "validate-frame" ? "validation.json" :
                          command == "command-lists" ? "command-lists.json" : "inventory.json";
    QSaveFile file(output.filePath(filename));
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open DX12 inspection output");
    const auto write = [&](const std::string &s) {
        if (file.write(s.data(), qint64(s.size())) != qint64(s.size())) throw std::runtime_error("Cannot write DX12 inspection output");
    };
    auto report = session->information();
    report["status"] = command == "validate-frame" ? "blocked" : "indexed";
    report["note"] = "Stream indexing does not establish native replay support";
    if (options.isSet("check-device") || options.isSet("check-copy") || options.isSet("check-pipeline") || options.isSet("check-initialization")) {
        try {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            std::optional<dx12::InitializationCopy> copy;
            std::optional<uint64_t> pipelineRecord;
            if (options.isSet("check-pipeline")) {
                bool valid = false; const auto record = options.value("check-pipeline").toULongLong(&valid);
                if (!valid || !record || record > directory.packets().size()) throw std::runtime_error("Invalid pipeline creation record");
                const auto &packet = directory.packets()[size_t(record - 1)];
                (void)dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
                pipelineRecord = record;
            }
            if (options.isSet("check-copy")) {
                bool validRecord = false, validLimit = false;
                const auto record = options.value("check-copy").toULongLong(&validRecord);
                const auto limit = options.value("max-bytes").toULongLong(&validLimit);
                if (!validRecord || !record || !validLimit || !limit) throw std::runtime_error("Invalid copy record or memory budget");
                copy = dx12::prepareInitializationCopy(directory, record, limit);
            }
            std::optional<RenderDocCapture> analysisCapture;
            if (options.isSet("renderdoc")) analysisCapture.emplace(std::filesystem::path(options.value("renderdoc").toStdWString()),
                std::filesystem::path(output.filePath("native-dx12").toStdWString()), false);
            dx12::NativeDevice device(directory, options.isSet("debug-device"));
            report["device"] = device.information();
            if (copy) report["initialization_copy"] = dx12::executeInitializationCopy(device, *copy);
            if (pipelineRecord) {
                dx12::NativePipelines pipelines(device, directory); pipelines.create(*pipelineRecord);
                report["pipeline"] = pipelines.information(*pipelineRecord);
            }
            if (options.isSet("check-initialization")) {
                const auto number = [&](const char *name) {
                    bool valid = false; const auto value = options.value(name).toULongLong(&valid);
                    if (!valid) throw std::runtime_error(std::string("Invalid unsigned integer: --") + name);
                    return uint64_t(value);
                };
                const auto limit = number("max-bytes");
                const auto id = options.isSet("id") ? number("id") : 0;
                const auto offset = number("offset");
                const auto length = options.isSet("length") ? number("length") : 0;
                const auto textureCopy = options.isSet("verify-texture-copy") ? number("verify-texture-copy") : 0;
                const auto execution = options.isSet("check-execution") ? number("check-execution") : 0;
                const auto texture = options.isSet("texture-object") ? number("texture-object") : 0;
                const auto subresource = number("subresource");
                if ((options.isSet("check-execution") && !execution) || (!execution && (texture || options.isSet("before"))) ||
                    (options.isSet("texture-object") && !texture) || subresource > UINT_MAX || (!texture && options.isSet("subresource")))
                    throw std::runtime_error("Frame inspection requires a nonzero execution; texture inspection also requires an object and a valid subresource");
                if (!limit || (options.isSet("id") && (!id || !length)) || (!id && (options.isSet("offset") || options.isSet("length"))))
                    throw std::runtime_error("Initialization requires a nonzero memory budget; buffer inspection requires --id and --length");
                if (id) {
                    if (id > directory.initializationCount()) throw std::runtime_error("Buffer creation is outside initialization");
                    const auto &packet = directory.packets()[size_t(id - 1)]; const auto recipe = dx12::decodeResourceRecipe(packet.type, directory.payload(packet));
                    const auto &desc = recipe.description;
                    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || offset > desc.Width || length > desc.Width - offset)
                        throw std::runtime_error("Invalid initialized buffer inspection range");
                }
                if (options.isSet("verify-texture-copy") && (!textureCopy || textureCopy > directory.initializationCount() ||
                    directory.packets()[size_t(textureCopy - 1)].type != 0x1ae3c53697cd3ULL))
                    throw std::runtime_error("Invalid initialization texture copy record");
                dx12::NativeReplay replay(device, directory, limit);
                std::string stage = "initialization";
                try {
                    if (execution) replay.prepareExecution(execution, !options.isSet("before"), bool(analysisCapture));
                    replay.initializeTransfers();
                    stage = "buffer_readback";
                    if (id) report["initialized_buffer"] = replay.inspectBuffer(id, offset, length);
                    stage = "texture_readback";
                    if (textureCopy) report["initialized_texture"] = replay.verifyInitializedTexture(textureCopy);
                    stage = "frame_execution";
                    if (analysisCapture) analysisCapture->start(device.device(), "FloraGPA independent DX12 replay");
                    if (execution) replay.replayTo(execution, !options.isSet("before"), bool(analysisCapture));
                    if (analysisCapture) {
                        stage = "analysis_capture";
                        const auto captured = analysisCapture->finish();
                        report["analysis_capture"] = {{"backend", "dx12"}, {"path", QString::fromStdWString(captured.wstring()).toStdString()},
                            {"execution", std::to_string(execution)}, {"boundary", options.isSet("before") ? "before" : "after"}};
                    }
                    stage = "frame_texture_readback";
                    if (texture) report["frame_texture"] = replay.readTexture(texture, UINT(subresource), std::filesystem::path(output.filePath("texture.bin").toStdWString()));
                    report["initialization"] = replay.information();
                } catch (const std::exception &e) {
                    // Discard while the native device is alive. The capture hook
                    // was loaded before device creation and is destroyed later.
                    if (analysisCapture) analysisCapture->discard();
                    report["initialization"] = replay.information();
                    report["initialization"]["failure"] = e.what();
                    report["initialization"]["failure_stage"] = stage;
                }
                report["device"] = device.information();
            }
        } catch (const std::exception &e) {
            report[options.isSet("check-initialization") ? "initialization" : options.isSet("check-copy") ? "initialization_copy" : options.isSet("check-pipeline") ? "pipeline" : "device"] = {{"status", "failed"}, {"reason", e.what()}};
        }
    }
    if (command == "commands") {
        write("[\n"); bool first = true;
        for (auto id : session->events()) {
            const auto event = session->event(id);
            if (!QString::fromStdString(event.name + " " + event.summary + " " + std::to_string(id))
                     .contains(options.value("filter"), Qt::CaseInsensitive)) continue;
            if (!first) write(",\n"); first = false;
            write(nlohmann::json{{"id", std::to_string(id)}, {"name", event.name}, {"summary", event.summary}}.dump());
        }
        write("\n]\n");
    } else if (command == "command-lists") write(session->submissionDetails().dump(2) + "\n");
    else write(report.dump(2) + "\n");
    if (!file.commit()) throw std::runtime_error("Cannot publish DX12 inspection output");
    QTextStream(stdout) << QString::fromStdString(report.dump(2)) << Qt::endl;
    return command == "validate-frame" ? 2 : 0;
}
}
