#include "Dx12Cli.h"
#include "application/ReplaySession.h"
#include "dx12/NativeDevice.h"
#include "dx12/InitializationCopy.h"
#include "dx12/PipelineRecipes.h"
#include "dx12/NativeReplay.h"
#include <QDir>
#include <QSaveFile>
#include <QTextStream>

namespace flora {
int runDx12Cli(const QString &command, const QString &path, const QCommandLineParser &options) {
    if (command != "inventory" && command != "commands" && command != "validate-frame" && command != "command-lists")
        throw std::runtime_error("DX12 " + command.toStdString() + " is not implemented; use inventory or commands for stream inspection");
    for (const auto &option : options.optionNames())
        if (option != "out" && option != "replay-identity" && !(command == "commands" && option == "filter") &&
            !(command == "validate-frame" && (option == "check-device" || option == "check-copy" || option == "check-pipeline" || option == "check-initialization" ||
                (option == "max-bytes" && (options.isSet("check-copy") || options.isSet("check-initialization"))) ||
                (options.isSet("check-initialization") && (option == "id" || option == "offset" || option == "length" || option == "verify-texture-copy" ||
                    option == "check-execution" || option == "texture-object" || option == "subresource" || option == "before" || option == "debug-device")))))
            throw std::runtime_error("Unsupported DX12 inspection option: --" + option.toStdString());
    if (options.value("out").isEmpty()) throw std::runtime_error("DX12 inspection requires --out");
    if (int(options.isSet("check-copy")) + int(options.isSet("check-pipeline")) + int(options.isSet("check-initialization")) > 1)
        throw std::runtime_error("Choose one targeted initialization check per invocation");
    const QDir output(options.value("out"));
    if (output.exists() && !output.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).empty())
        throw std::runtime_error("Output directory must be new or empty");
    auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
    if (!QDir().mkpath(output.path())) throw std::runtime_error("Cannot create DX12 inspection output directory");
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
                    replay.initializeTransfers();
                    stage = "buffer_readback";
                    if (id) report["initialized_buffer"] = replay.inspectBuffer(id, offset, length);
                    stage = "texture_readback";
                    if (textureCopy) report["initialized_texture"] = replay.verifyInitializedTexture(textureCopy);
                    stage = "frame_execution";
                    if (execution) replay.replayTo(execution, !options.isSet("before"));
                    stage = "frame_texture_readback";
                    if (texture) report["frame_texture"] = replay.readTexture(texture, UINT(subresource), std::filesystem::path(output.filePath("texture.bin").toStdWString()));
                    report["initialization"] = replay.information();
                } catch (const std::exception &e) {
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
