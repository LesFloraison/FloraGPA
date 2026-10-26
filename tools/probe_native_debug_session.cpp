#include "application/NativeDebugSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <iostream>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Cannot read bounded session metadata");
    return Json::parse(file.readAll().toStdString());
}
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto args = app.arguments();
        require(args.size() == 4,
                "Usage: FloraNativeDebugSessionProbe <job-directory> <report.json> <archive.zip>");
        QElapsedTimer timer;
        timer.start();
        const QDir directory(args[1]);
        const auto job = read(directory.filePath("job.json")),
                   result = read(directory.filePath("result.json"));
        auto artifacts = readNativeArtifacts(directory.path(), result.at("artifacts"), 256 * 1024 * 1024);
        auto session = std::make_unique<NativeDebugSession>(job, result, artifacts);
        artifacts.clear();
        const auto loadedMs = timer.elapsed();
        const auto path = session->tracePath();
        auto &model = session->model();
        const auto original = model.registerValues();
        size_t mapped = 0;
        for (size_t i = 0; i < model.size(); ++i) {
            model.move(i);
            mapped += !model.location().is_null();
        }
        model.move(model.size());
        const auto terminal = model.instructionState();
        model.move(0);
        require(model.registerValues() == original, "Session register restoration mismatch");
        bool resumed = false;
        if (QFileInfo::exists(directory.filePath("state.json"))) {
            const auto state = read(directory.filePath("state.json"));
            model.importConfiguration(state.at("configuration"));
            model.move(state.at("position"));
            require(model.registerValues() == state.at("registers") &&
                        model.instructionState() == state.at("state"),
                    "Saved session state mismatch");
            resumed = true;
        }
        session->exportArchive(args[3], model.exportReport());
        const Json report{{"format", "flora.native-debug-session-probe"},
                          {"version", 1},
                          {"passed", true},
                          {"context_id", result.at("context_id")},
                          {"selected_steps", model.size()},
                          {"mapped_steps", mapped},
                          {"terminal", terminal},
                          {"register_round_trip", true},
                          {"resumed", resumed},
                          {"load_ms", loadedMs},
                          {"total_ms", timer.elapsed()}};
        session.reset();
        require(!QFileInfo::exists(path), "Released session retained its temporary journal");
        QSaveFile file(args[2]);
        const auto bytes = QByteArray::fromStdString(report.dump(2));
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
                "Cannot publish session probe report");
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
