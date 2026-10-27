#include <Windows.h>

#include "app/ReplayMeshView.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeReplayMesh.h"
#include <Psapi.h>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <iostream>
using namespace flora;
using Json = nlohmann::json;
namespace {
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read mesh probe artifact");
    return file.readAll();
}
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot save mesh probe evidence");
}
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
Json processEvidence() {
    std::vector<HMODULE> handles(256);
    DWORD bytes = 0;
    for (;;) {
        require(EnumProcessModules(GetCurrentProcess(), handles.data(),
                                   DWORD(handles.size() * sizeof(HMODULE)), &bytes),
                "Cannot enumerate probe modules");
        if (bytes <= handles.size() * sizeof(HMODULE))
            break;
        handles.resize(bytes / sizeof(HMODULE) + 64);
    }
    Json modules = Json::array();
    for (size_t i = 0; i < bytes / sizeof(HMODULE); ++i) {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(handles[i], path, DWORD(std::size(path)));
        require(length && length < std::size(path), "Cannot read probe module path");
        modules.push_back(QString::fromWCharArray(path, length).toStdString());
    }
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof memory;
    require(GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof memory),
            "Cannot read probe memory usage");
    return {{"loaded_modules", modules}, {"peak_working_set_bytes", memory.PeakWorkingSetSize}};
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        require(args.size() == 6,
                "Usage: FloraNativeMeshCaptureProbe FRAME EVENT INSTANCE VENDOR NEW_DIRECTORY");
        auto number = [&](int i) {
            bool ok = false;
            const auto value = args[i].toULongLong(&ok, 0);
            require(ok && !args[i].startsWith('-'), "Invalid probe selection");
            return value;
        };
        const auto event = number(2), instance = number(3), vendor = number(4);
        require(event && instance <= UINT32_MAX && vendor <= UINT16_MAX, "Probe selection out of range");
        QDir output(args[5]);
        require(!output.exists() && QDir().mkpath(args[5]), "Probe directory must be new");
        QTemporaryDir settings;
        require(settings.isValid(), "Cannot isolate probe settings");
        QCoreApplication::setOrganizationName("FloraGPATests");
        QCoreApplication::setApplicationName("NativeMeshCaptureProbe");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
        QElapsedTimer elapsed;
        elapsed.start();
        Json records = Json::array();
        for (const auto stage : {"vs-inputs", "final"}) {
            QElapsedTimer stageElapsed;
            stageElapsed.start();
            ReplayMeshView view;
            view.setNativeMode(true);
            view.setContext("real-capture", event);
            view.findChild<QLineEdit *>("replayMeshInstance")->setText(QString::number(instance));
            auto options = view.request();
            options.erase("action");
            options["stage"] = stage;
            if (vendor)
                options["gpu_vendor"] = vendor;
            else
                options["warp"] = true;
            const auto directory = output.filePath(stage);
            const auto job = prepareNativeAnalysisJob(args[1], "mesh", directory, worker, options);
            const auto result = Json::parse(read(runNativeAnalysisJob(job, worker, 180)).toStdString());
            require(result.at("data").at("executed_stages") == Json::array({"vs"}),
                    "This comparison requires VS-only geometry; tessellation and GS need separate oracles");
            auto artifacts = readNativeArtifacts(directory, result.at("artifacts"), 512ull * 1024 * 1024);
            auto mesh = std::make_shared<NativeReplayMesh>(nativeReplayMesh(job, result, artifacts));
            require(!mesh->positions.positions.empty(), "Expected nonempty real-capture geometry");
            const bool present = std::string_view(stage) == "vs-inputs";
            if (present) {
                require(view.finishNative(view.requestId(), result, mesh), "UI rejected native VS result");
                const auto table = view.findChild<QTableView *>("replayMeshPositions");
                require(table && table->model()->rowCount() == int(mesh->positions.positions.size()),
                        "UI vertex table count mismatch");
                for (const auto extension : {"csv", "obj", "json"})
                    view.exportResult(QDir(directory).filePath(QString("ui-export.") + extension));
                require(read(QDir(directory).filePath("ui-export.csv")) == mesh->files["post_vertices.csv"] &&
                            read(QDir(directory).filePath("ui-export.obj")) ==
                                mesh->files["post_geometry.obj"],
                        "UI exports differ from retained mesh");
                const auto exported =
                    Json::parse(read(QDir(directory).filePath("ui-export.json")).toStdString());
                require(exported.at("presentation") == mesh->tables, "UI JSON presentation mismatch");
            }
            save(QDir(directory).filePath("post_vertices.csv"), mesh->files["post_vertices.csv"]);
            save(QDir(directory).filePath("post_geometry.obj"), mesh->files["post_geometry.obj"]);
            save(QDir(directory).filePath("presentation.json"),
                 QByteArray::fromStdString(mesh->tables.dump(2)));
            records.push_back({{"stage", stage},
                               {"ui_presented", present},
                               {"vertices", mesh->positions.positions.size()},
                               {"faces", mesh->positions.faces.size()},
                               {"elapsed_ms", stageElapsed.elapsed()},
                               {"context_id", mesh->contextId}});
        }
        const Json report{{"schema", 1},
                          {"completed", true},
                          {"event", event},
                          {"instance", instance},
                          {"vendor", vendor},
                          {"records", records},
                          {"elapsed_ms", elapsed.elapsed()},
                          {"process", processEvidence()}};
        save(output.filePath("probe.json"), QByteArray::fromStdString(report.dump(2)));
        std::cout << report.dump() << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << Json{{"completed", false}, {"error", error.what()}}.dump() << std::endl;
        return 1;
    }
}
