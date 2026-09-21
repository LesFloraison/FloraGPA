#include "DepthStencilCapture.h"
#include "application/Experiment.h"
#include "application/Quad.h"
#include "application/QuadResources.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json run(const Json &job) {
    Frame capture(QString::fromStdString(job.at("capture").get<std::string>()).toStdWString());
    ReplayOptions options;
    options.until = job.value("event", Id(100));
    options.warp = job.value("warp", false);
    if (job.contains("experiment")) {
        Experiment project(capture);
        project.load(QString::fromStdString(job.at("experiment").get<std::string>()), capture);
        project.apply(capture, options);
    }
    for (const auto &id : job.value("disabled", Json::array()))
        options.disabled.insert(id.get<Id>());
    const auto &frame = effectiveFrame(capture, options);
    const auto event = frame.event(options.until);
    const auto state = effectiveBindings(frame, event.id, frame.state(event.state), options);
    QuadOptions request;
    request.depthMode = job.value("depth", "prepared");
    request.target = job.value("target", "auto");
    if (job.contains("layer"))
        request.layer = job.at("layer").get<uint32_t>();
    Replay replay(frame, options);
    const auto result = captureQuad(replay, event.id, request);
    Json actual{{"report", result.report},
                {"counter_report", result.counterReport},
                {"preview_sha256", sha256(result.preview.rgba)},
                {"storage_sha256", Json::array()}};
    for (const auto &data : result.storage)
        actual["storage_sha256"].push_back(sha256(data));
    actual["after"] = QuadResources(replay).fingerprint(state);
    Replay baseline(frame, options);
    baseline.run();
    actual["baseline_unchanged"] =
        actual["after"] == QuadResources(baseline).fingerprint(state) && replay.counts == baseline.counts;
    if (job.contains("out"))
        exportQuad(result, QString::fromStdString(job.at("out").get<std::string>()).toStdWString());
    return actual;
}
} // namespace
class QuadTests final : public QObject {
    Q_OBJECT
  private slots:
    void counterAndOriginalDraw() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        for (bool warp : {false, true})
            for (const auto *mode : {"prepared", "before", "none"}) {
                const auto result =
                    run({{"capture", path.toStdString()}, {"event", 1000}, {"warp", warp}, {"depth", mode}});
                QCOMPARE(result.at("baseline_unchanged"), Json(true));
                const auto &report = result.at("report");
                QCOMPARE(report.at("original_storage_unchanged"), Json(true));
                QCOMPARE(report.at("histogram_accounting_matches_reference"), Json(true));
                QCOMPARE(report.at("unresolved_locks"), Json(0));
                QCOMPARE(report.at("residual_live"), Json(0));
            }
    }
    void boundCounterWithoutStorageVerification() {
        QTemporaryDir directory;
        const auto path = directory.filePath("triangle.gpa_frame");
        testing::depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions options;
            options.until = 1000;
            options.before = true;
            options.warp = warp;
            Replay replay(frame, options);
            bool observed = false;
            replay.run({}, [&](Id id, bool after, auto *, const auto &) {
                if (id != options.until || after)
                    return;
                const auto event = frame.event(id);
                const auto state = frame.state(event.state);
                QuadResources resources(replay);
                const auto before = resources.fingerprint(state);
                const auto counts = replay.counts;
                QuadCounterOptions request;
                request.verifyStorage = false;
                const auto result = diagnoseQuadBound(replay, event, state, request);
                QCOMPARE(resources.fingerprint(state), before);
                QCOMPARE(replay.counts, counts);
                const auto &report = result.report;
                QVERIFY(report.at("original_storage_unchanged").is_null());
                QVERIFY(report.at("serialization").is_null());
                QCOMPARE(report.at("counter_submissions"), Json(1));
                QCOMPARE(report.at("reference_submissions"), Json(1));
                QCOMPARE(report.at("histogram_accounting_matches_reference"), Json(true));
                QCOMPARE(report.at("unresolved_locks"), Json(0));
                request.prepareDepth = true;
                request.depthTest = false;
                QVERIFY_EXCEPTION_THROWN(diagnoseQuadBound(replay, event, state, request),
                                         std::runtime_error);
                QCOMPARE(resources.fingerprint(state), before);
                observed = true;
            });
            QVERIFY(observed);
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        Json result = Json::array();
        for (const auto &job : Json::parse(input.readAll().toStdString()))
            try {
                result.push_back(run(job));
            } catch (const std::exception &error) {
                result.push_back({{"error", error.what()}});
            }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(result.dump(2)));
        return 0;
    }
    QuadTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "QuadTests.moc"
