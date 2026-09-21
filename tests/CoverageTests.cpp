#include "DepthStencilCapture.h"
#include "application/Coverage.h"
#include "application/Experiment.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json run(const Json &job) {
    Frame frame(QString::fromStdString(job.at("capture").get<std::string>()).toStdWString());
    const auto id = job.value("event", Id(100));
    ReplayOptions options;
    options.until = id;
    options.warp = job.value("warp", false);
    for (auto value : job.value("disabled", Json::array()))
        options.disabled.insert(value.get<Id>());
    if (job.contains("experiment")) {
        Experiment project(frame);
        project.load(QString::fromStdString(job.at("experiment").get<std::string>()), frame);
        project.apply(frame, options);
    }
    CoverageOptions request;
    request.mode = job.value("mode", "fragment");
    request.target = job.value("target", "auto");
    request.depthTest = job.value("depth_test", true);
    if (job.contains("layer") && !job["layer"].is_null())
        request.layer = job["layer"].get<uint32_t>();
    Replay replay(frame, options);
    Json result;
    if (job.value("baseline", false))
        replay.run();
    else {
        auto coverage = captureCoverage(replay, id, request);
        result["report"] = coverage.report;
        result["mask"] = sha256(coverage.mask.rgba);
        result["after"] = sha256(coverage.after.rgba);
        result["overlay"] = sha256(coverage.overlay.rgba);
        result["size"] = {coverage.mask.width, coverage.mask.height};
        if (job.contains("out"))
            exportCoverage(coverage, QString::fromStdString(job.at("out").get<std::string>()).toStdWString());
    }
    result["storage"] = Json::object();
    for (auto value : job.value("buffers", Json::array())) {
        auto resource = value.get<Id>();
        result["storage"][std::to_string(resource)] = sha256(replay.readBuffer(resource));
    }
    for (auto value : job.value("textures", Json::array())) {
        auto resource = value.get<Id>();
        result["storage"][std::to_string(resource)] = sha256(replay.readTexture(resource));
    }
    result["counters"] = Json::object();
    for (auto value : job.value("counters", Json::array()))
        result["counters"][std::to_string(value.get<Id>())] = replay.readCounter(value.get<Id>());
    result["counts"] = replay.counts;
    result["so_history"] = Json::array();
    for (const auto &row : replay.streamOutputHistory)
        result["so_history"].push_back({{"event", row.event},
                                        {"stream", row.stream},
                                        {"primitives_written", row.written},
                                        {"primitives_storage_needed", row.needed},
                                        {"vertices_per_primitive", row.factor}});
    result["msaa"] = Json::object();
    for (const auto &value : job.value("msaa", Json::array())) {
        const auto resource = value.at("id").get<Id>();
        auto &samples = result["msaa"][std::to_string(resource)];
        samples = Json::array();
        for (uint32_t s = 0; s < value.at("samples").get<uint32_t>(); ++s)
            samples.push_back(sha256(replay.readMsaa(resource, s, value.at("format").get<uint32_t>()).bytes));
    }
    if (!job.value("baseline", false) && (request.depthTest || request.mode == "geometry")) {
        auto baselineJob = job;
        baselineJob["baseline"] = true;
        const auto baseline = run(baselineJob);
        result["preserves_original"] =
            result["so_history"] == baseline["so_history"] && result["counts"] == baseline["counts"] &&
            result["storage"] == baseline["storage"] && result["counters"] == baseline["counters"] &&
            result["msaa"] == baseline["msaa"];
    }
    return result;
}
} // namespace
class CoverageTests final : public QObject {
    Q_OBJECT
  private slots:
    void validationAndRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.until = 1000;
        Replay replay(frame, options);
        CoverageOptions request;
        request.target = "rt7";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, captureCoverage(replay, 1000, request));
        QCOMPARE(replay.generation(), 0ull);
        request.target = "auto";
        request.layer = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, captureCoverage(replay, 1000, request));
        request.layer.reset();
        auto result = captureCoverage(replay, 1000, request);
        Replay baseline(frame, options);
        baseline.run();
        QCOMPARE(replay.readTexture(20), baseline.readTexture(20));
        QCOMPARE(replay.readTexture(720), baseline.readTexture(720));
        QCOMPARE(replay.counts, baseline.counts);
        exportCoverage(result, dir.filePath("export").toStdWString());
        for (auto name : {"coverage.png", "after_draw.png", "overlay.png", "coverage.json"})
            QVERIFY(QFile::exists(dir.filePath(QString("export/") + name)));
    }
    void predicatedDraw() {
        QTemporaryDir dir;
        for (bool visible : {false, true}) {
            auto path = dir.filePath(QString("predicate-%1.gpa_frame").arg(visible));
            testing::predicateCapture(visible).save(path);
            Frame frame(path.toStdWString());
            for (bool warp : {false, true}) {
                ReplayOptions options;
                options.until = 2000;
                options.warp = warp;
                Replay baseline(frame, options);
                baseline.run();
                for (auto mode : {"fragment", "geometry"}) {
                    Replay replay(frame, options);
                    CoverageOptions request;
                    request.mode = mode;
                    auto result = captureCoverage(replay, 2000, request);
                    QCOMPARE(replay.readTexture(20), baseline.readTexture(20));
                    QCOMPARE(replay.counts, baseline.counts);
                    QCOMPARE(replay.readPredicateResult(600).value, baseline.readPredicateResult(600).value);
                    QCOMPARE(result.report["covered_pixels"], Json(visible ? 1 : 0));
                }
            }
        }
    }
    void depthAndDisabled() {
        QTemporaryDir dir;
        auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions options;
            options.warp = warp;
            options.until = 1000;
            Replay baseline(frame, options);
            baseline.run();
            for (const auto mode : {"fragment", "geometry"}) {
                Replay replay(frame, options);
                CoverageOptions request;
                request.mode = mode;
                auto result = captureCoverage(replay, 1000, request);
                QCOMPARE(replay.readTexture(20), baseline.readTexture(20));
                QCOMPARE(replay.readTexture(720), baseline.readTexture(720));
                QCOMPARE(replay.counts, baseline.counts);
                QCOMPARE(result.mask.width, 1u);
                QCOMPARE(result.mask.height, 1u);
                QCOMPARE(result.report["original_submissions"], Json(1));
                QCOMPARE(result.report["diagnostic_submissions"],
                         Json(mode == std::string("geometry") ? 1 : 0));
            }
            options.disabled.insert(1000);
            Replay replay(frame, options);
            auto result = captureCoverage(replay, 1000);
            QCOMPARE(result.report["covered_pixels"], Json(0));
            QCOMPARE(result.report["submissions"], Json(0));
        }
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]), output(args[3]);
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        auto jobs = Json::parse(input.readAll().toStdString());
        Json results = Json::array();
        for (const auto &job : jobs) {
            try {
                results.push_back(run(job));
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        }
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(results.dump(2)));
        return 0;
    }
    CoverageTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "CoverageTests.moc"
