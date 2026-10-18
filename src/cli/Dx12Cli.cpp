#include "Dx12Cli.h"
#include "application/ReplaySession.h"
#include "application/Experiment.h"
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
#include <set>

namespace flora {
namespace {
int replayDx12(const IReplaySession &session, const QString &path, const QCommandLineParser &options,
               const QDir &output) {
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
    ReplaySelection selection;
    selection.session = session.identity();
    selection.execution = execution;
    selection.resource = creation;
    const auto capability = session.capability(ReplayFeature::Replay, selection);
    if (!capability.supported())
        throw std::runtime_error(capability.reason);
    std::optional<Experiment> experiment;
    if (options.isSet("experiment") || options.isSet("disable")) {
        experiment.emplace(session);
        if (options.isSet("experiment")) experiment->load(options.value("experiment"), session);
        if (options.isSet("disable")) for (const auto &id : options.value("disable").split(',')) {
            bool valid{};
            const auto editedExecution = id.toULongLong(&valid);
            if (!valid || !editedExecution || id != QString::number(editedExecution))
                throw std::runtime_error("DX12 --disable requires comma-separated decimal execution IDs");
            experiment->setEnabled(session, editedExecution, false);
        }
        experiment->save(output.filePath("experiment.json"));
    }
    dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
    std::optional<RenderDocCapture> capture;
    if (options.isSet("renderdoc"))
        capture.emplace(std::filesystem::path(options.value("renderdoc").toStdWString()),
                        std::filesystem::path(output.filePath("native-dx12").toStdWString()), false);
    dx12::NativeDevice device(directory, options.isSet("debug-device"));
    dx12::NativeReplay replay(device, directory, budget);
    if (experiment) replay.setDisabledExecutions(experiment->disabledExecutions(session));
    nlohmann::json report{{"backend", "dx12"},
                          {"completed", false},
                          {"frame_complete", false},
                          {"replay_scope", "selected_execution"},
                          {"execution", std::to_string(execution)},
                          {"event", std::to_string(execution)},
                          {"value_time", options.isSet("before") ? "before_event" : "after_event"},
                          {"image_available", false},
                          {"width", nullptr},
                          {"height", nullptr},
                          {"resource", nullptr},
                          {"rgba_sha256", nullptr}};
    if (experiment) {
        const auto document = QByteArray::fromStdString(experiment->document().dump());
        report["experiment"] = {{"backend", "dx12"}, {"capture_identity", experiment->document().at("capture_identity")},
            {"cursor", std::to_string(experiment->revision())},
            {"document_sha256", QCryptographicHash::hash(document, QCryptographicHash::Sha256).toHex().toStdString()}};
    }
    try {
        replay.prepareExecution(execution, !options.isSet("before"), bool(capture), options.isSet("measure-event"));
        replay.initializeTransfers();
        if (capture)
            capture->start(device.device(), "FloraGPA independent DX12 replay");
        replay.replayTo(execution, !options.isSet("before"), bool(capture));
        if (options.isSet("measure-event"))
            report["performance"] = replay.performanceResult();
        if (capture)
            report["rdc_capture"] = QString::fromStdWString(capture->finish().wstring()).toStdString();
        if (object) {
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
        report["image_status"] = object ? (report["image_available"] == true
                                               ? "Native GPU readback; selected execution only"
                                               : "Raw GPU texture saved; this display format is unsupported")
                                        : "Execution replayed; select a texture creation record for readback";
    } catch (...) {
        if (capture)
            capture->discard();
        throw;
    }
    report["device"] = device.information();
    report["native_replay"] = replay.information();
    QSaveFile file(output.filePath("report.json"));
    const auto bytes = QByteArray::fromStdString(report.dump(2));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save DX12 replay report");
    QTextStream(stdout) << QString::fromUtf8(bytes) << Qt::endl;
    return 0;
}
} // namespace
int runDx12Cli(const QString &command, const QString &path, const QCommandLineParser &options) {
    if (command != "inventory" && command != "commands" && command != "validate-frame" &&
        command != "command-lists" && command != "replay")
        throw std::runtime_error("DX12 " + command.toStdString() +
                                 " is not implemented; use inventory or commands for stream inspection");
    std::set<QString> allowed{"out", "replay-identity"};
    if (command == "commands")
        allowed.insert("filter");
    if (command == "replay")
        allowed.insert({"event", "before", "id", "subresource", "max-bytes", "renderdoc", "debug-device", "measure-event", "experiment", "disable"});
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
    if (command == "replay") return replayDx12(*session,path,options,output);
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
