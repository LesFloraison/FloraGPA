#include "Dx12Cli.h"
#include "application/ReplaySession.h"
#include "dx12/NativeDevice.h"
#include "dx12/InitializationCopy.h"
#include <QDir>
#include <QSaveFile>
#include <QTextStream>

namespace flora {
int runDx12Cli(const QString &command, const QString &path, const QCommandLineParser &options) {
    if (command != "inventory" && command != "commands" && command != "validate-frame" && command != "command-lists")
        throw std::runtime_error("DX12 " + command.toStdString() + " is not implemented; use inventory or commands for stream inspection");
    for (const auto &option : options.optionNames())
        if (option != "out" && !(command == "commands" && option == "filter") &&
            !(command == "validate-frame" && (option == "check-device" || option == "check-copy" ||
                (option == "max-bytes" && options.isSet("check-copy")))))
            throw std::runtime_error("Unsupported DX12 inspection option: --" + option.toStdString());
    if (options.value("out").isEmpty()) throw std::runtime_error("DX12 inspection requires --out");
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
    if (options.isSet("check-device") || options.isSet("check-copy")) {
        try {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            std::optional<dx12::InitializationCopy> copy;
            if (options.isSet("check-copy")) {
                bool validRecord = false, validLimit = false;
                const auto record = options.value("check-copy").toULongLong(&validRecord);
                const auto limit = options.value("max-bytes").toULongLong(&validLimit);
                if (!validRecord || !record || !validLimit || !limit) throw std::runtime_error("Invalid copy record or memory budget");
                copy = dx12::prepareInitializationCopy(directory, record, limit);
            }
            dx12::NativeDevice device(directory);
            report["device"] = device.information();
            if (copy) report["initialization_copy"] = dx12::executeInitializationCopy(device, *copy);
        } catch (const std::exception &e) {
            report[options.isSet("check-copy") ? "initialization_copy" : "device"] = {{"status", "failed"}, {"reason", e.what()}};
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
