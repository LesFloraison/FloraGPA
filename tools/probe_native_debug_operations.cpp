#include "app/NativeDebugOperationController.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSaveFile>
#include <QTimer>
#include <iostream>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Cannot read bounded operation metadata");
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
                "Usage: FloraNativeDebugOperationProbe <job-directory> <report.json> <archive.zip>");
        const QDir directory(args[1]);
        const auto job = read(directory.filePath("job.json")),
                   result = read(directory.filePath("result.json"));
        auto artifacts = readNativeArtifacts(directory.path(), result.at("artifacts"), 256 * 1024 * 1024);
        auto session = std::make_shared<NativeDebugSession>(job, result, artifacts);
        artifacts.clear();
        const auto original = session->model().registerValues(), config = session->model().configuration();
        const auto terminal = session->model().size();
        Json operations = Json::array();
        auto run = [&](const char *name, size_t position, int cancelAfter,
                       const QString &archive = QString()) {
            NativeDebugOperationController controller;
            NativeDebugOperationOutcome outcome;
            QEventLoop loop;
            QElapsedTimer clock;
            QTimer beat, cancellation;
            beat.setTimerType(Qt::PreciseTimer);
            cancellation.setSingleShot(true);
            int beats = 0;
            qint64 previous = 0, maximumGap = 0;
            QObject::connect(&beat, &QTimer::timeout, &loop, [&] {
                const auto now = clock.nsecsElapsed();
                maximumGap = std::max(maximumGap, now - previous);
                previous = now;
                ++beats;
            });
            QObject::connect(&cancellation, &QTimer::timeout, &controller,
                             &NativeDebugOperationController::cancel);
            QObject::connect(&controller, &NativeDebugOperationController::finished, &loop,
                             [&](const auto &value) {
                                 outcome = value;
                                 loop.quit();
                             });
            clock.start();
            beat.start(1);
            controller.start(session, NativeDebugCommand::Position, position, config, "uint", archive);
            if (cancelAfter >= 0)
                cancellation.start(cancelAfter);
            loop.exec();
            const auto elapsed = clock.nsecsElapsed();
            maximumGap = std::max(maximumGap, elapsed - previous);
            require(outcome.error.isEmpty() || outcome.cancelled,
                    "Unexpected native debug operation failure");
            if (cancelAfter < 0)
                require(!outcome.cancelled, "Unexpected operation cancellation");
            require(elapsed < 180000000000ll, "Native debug operation exceeded its deadline");
            operations.push_back({{"name", name},
                                  {"elapsed_ms", double(elapsed) / 1000000},
                                  {"heartbeat_count", beats},
                                  {"maximum_heartbeat_gap_ms", double(maximumGap) / 1000000},
                                  {"cancelled", outcome.cancelled},
                                  {"position", session->model().position()},
                                  {"exported", !outcome.exported.isEmpty()}});
            return outcome.cancelled;
        };
        run("terminal", terminal, -1);
        const bool cancelled = run("cancel_rewind", 0, 5);
        if (cancelled)
            require(session->model().position() == terminal, "Cancelled operation changed display position");
        run("rewind", 0, -1);
        require(session->model().registerValues() == original,
                "Native operation register restoration mismatch");
        run("export", 0, -1, args[3]);
        const Json report{{"format", "flora.native-debug-operation-probe"},
                          {"version", 1},
                          {"passed", true},
                          {"complete_release_acceptance", false},
                          {"context_id", result.at("context_id")},
                          {"selected_steps", terminal},
                          {"operations", operations},
                          {"register_round_trip", true}};
        QSaveFile file(args[2]);
        const auto bytes = QByteArray::fromStdString(report.dump(2));
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
                "Cannot publish operation probe report");
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
