#include "BlendCapture.h"
#include "application/Experiment.h"
#include "application/NativeAnalysisJobs.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
QString worker() { return QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe"; }
Json load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read draw history job evidence");
    return Json::parse(file.readAll().toStdString());
}
Json selection(uint32_t vendor = 0) {
    Json value{
        {"history_scope", "private-draw"}, {"gpa_event", 100}, {"resource", 1000}, {"debug_device", true}};
    if (vendor)
        value["gpu_vendor"] = vendor;
    else
        value["warp"] = true;
    return value;
}
void save(const QString &path, const Json &value) {
    QFile file(path);
    const auto text = value.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save draw history job evidence");
}
void evidence(const QString &source) {
    const auto root = qEnvironmentVariable("FLORA_DRAW_HISTORY_JOBS_EVIDENCE");
    if (root.isEmpty())
        return;
    QDir target(root + "/" + QString::fromLatin1(QTest::currentDataTag()));
    if (target.exists() || !QDir().mkpath(target.path()))
        throw std::runtime_error("Draw history job evidence directory must be new");
    for (const auto &file : QDir(source).entryList(QDir::Files))
        if (!QFile::copy(QDir(source).filePath(file), target.filePath(file)))
            throw std::runtime_error("Cannot preserve draw history job evidence");
}
} // namespace
class NativeDrawHistoryJobsTests final : public QObject {
    Q_OBJECT
  private slots:
    void validation() {
        const auto valid = selection();
        QCOMPARE(nativeAnalysisRequest("history", valid).options, valid);
        for (const auto &patch :
             {Json{{"gpa_event", nullptr}}, Json{{"history_scope", "events"}}, Json{{"shader_outputs", 1}},
              Json{{"output_merger", "true"}}, Json{{"primitive_identity", nullptr}},
              Json{{"primitive_presence", 1}}, Json{{"max_steps", 5}}}) {
            auto invalid = valid;
            invalid.update(patch);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", invalid));
        }
        auto missing = valid;
        missing.erase("gpa_event");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", missing));
        for (const auto key :
             {"shader_outputs", "output_merger", "primitive_identity", "primitive_presence"}) {
            auto events = Json{{"resource", 1000}, {"history_scope", "events"}, {key, false}};
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", events));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("texture", events));
        }
    }
    void isolatedDraw_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"storage", "shader", "merger", "identity", "presence", "all-msaa"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void isolatedDraw() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            QTemporaryDir dir;
            const auto capture = dir.filePath("frame.gpa_frame");
            const bool msaa = mode == "all-msaa";
            auto fixture = blendCapture(1, false, false, msaa ? 4 : 1);
            for (const auto &entry : fixture.entries)
                if (entry.id == 100)
                    put(fixture.bytes, size_t(entry.offset) + 24, 9u);
            fixture.save(capture);
            Frame frame(capture.toStdWString());
            Experiment experiment(frame);
            const std::string vs = "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;"
                                   "return float4(i==2?3:-1,i==1?3:-1,0,1);}";
            const std::string ps = "float4 main():SV_Target{return float4(.25,.75,.125,.5);}";
            experiment.setShader(frame, 10, compileClassProgram(vs, "vs_5_0"), vs, "main");
            experiment.setShader(frame, 12, compileClassProgram(ps, "ps_5_0"), ps, "main");
            const auto project = dir.filePath("experiment.json");
            experiment.save(project);
            auto options = selection(vendor);
            if (mode == "shader")
                options["shader_outputs"] = true;
            if (mode == "merger" || msaa)
                options["output_merger"] = true;
            if (mode == "identity" || msaa)
                options["primitive_identity"] = true;
            if (mode == "presence" || msaa)
                options["primitive_presence"] = true;
            if (msaa)
                options["sample"] = 2;
            const auto output = dir.filePath("result");
            const auto job = prepareNativeAnalysisJob(capture, "history", output, worker(), options, project);
            const auto result = load(runNativeAnalysisJob(job, worker(), 60));
            const auto &data = result.at("data");
            QCOMPARE(data, load(output + "/history.json"));
            QCOMPARE(data.at("kind"), Json("private_pixel_draw_history"));
            QCOMPARE(data.at("frame_sha256"), result.at("context").at("frame_sha256"));
            QCOMPARE(data.at("event"), Json(100));
            QCOMPARE(data.at("selection").at("sample"), Json(msaa ? 2 : 0));
            QCOMPARE(data.at("fragment_history_available"), Json(false));
            QVERIFY(data.at("original_fragment_eligibility").is_null());
            QCOMPARE(data.at("observed_storage_restored"), Json(true));
            QCOMPARE(data.at("records").size(), size_t(3));
            QCOMPARE(data.at("after").at("bytes"), Json(statePack(.25f, .75f, .125f, .5f)));
            QCOMPARE(data.at("before").at("bytes"), Json(statePack(.125f, .5f, .25f, .25f)));
            QCOMPARE(data.at("before"), data.at("restored"));
            QCOMPARE(data.at("shader_evaluation").at("requested"),
                     Json(mode == "shader" || mode == "merger" || msaa));
            QCOMPARE(data.at("merger_evaluation").at("requested"), Json(mode == "merger" || msaa));
            QCOMPARE(data.at("primitive_identity_observation").at("requested"),
                     Json(mode == "identity" || msaa));
            QCOMPARE(data.at("primitive_presence_observation").at("requested"),
                     Json(mode == "presence" || msaa));
            if (mode == "presence" || msaa) {
                const auto &presence = data.at("primitive_presence_observation");
                QCOMPARE(presence.at("status"), Json("observed"));
                QCOMPARE(presence.at("observations").size(), size_t(3));
                for (const auto &observed : presence.at("observations")) {
                    QCOMPARE(observed.at("status"), Json("repeat_agreement"));
                    QCOMPARE(observed.at("color_completion_observed"), Json(true));
                }
            }
            if (mode == "identity" || msaa) {
                QCOMPARE(data.at("primitive_identity_observation").at("system_primitive_id"), Json(2));
                QCOMPARE(data.at("primitive_identity_association").at("matching_primitive_ordinals"),
                         Json::array({2}));
            }
            QCOMPARE(result.at("context").at("adapter").at("warp"), Json(vendor == 0));
            if (vendor)
                QCOMPARE(result.at("context").at("adapter").at("vendor"), Json(vendor));
            QCOMPARE(data.at("adapter").at("luid_low"), result.at("context").at("adapter").at("luid_low"));
            QCOMPARE(result.at("context").at("effective_experiment"), experiment.document());
            for (const auto &module : result.at("execution").at("loaded_modules")) {
                const auto path = QString::fromStdString(module.get<std::string>()).toLower();
                QVERIFY(!path.contains("renderdoc") && !path.contains("intelswtools\\gpa\\"));
            }
            auto stale = job;
            stale["options"]["primitive_identity"] = !options.value("primitive_identity", false);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateNativeAnalysisResult(stale, result));
            stale = job;
            stale["experiment_sha256"] = "changed";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateNativeAnalysisResult(stale, result));
            save(output + "/verified.json", {{"vendor_request", vendor},
                                             {"mode", mode.toStdString()},
                                             {"authored_before", statePack(.125f, .5f, .25f, .25f)},
                                             {"authored_after", statePack(.25f, .75f, .125f, .5f)}});
            evidence(output);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void failureAndRetry() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        blendCapture().save(capture);
        auto bad = selection();
        bad["x"] = 1;
        const auto failed =
            prepareNativeAnalysisJob(capture, "history", dir.filePath("failed"), worker(), bad);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(failed, worker(), 30));
        QVERIFY(!QFile::exists(dir.filePath("failed/result.json")));
        const auto cancelled =
            prepareNativeAnalysisJob(capture, "history", dir.filePath("cancelled"), worker(), selection());
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, runNativeAnalysisJob(cancelled, worker(), 30, [&] {
                                     return QDir(dir.filePath("cancelled")).exists();
                                 }));
        QVERIFY(!QFile::exists(dir.filePath("cancelled/result.json")));
        const auto retry =
            prepareNativeAnalysisJob(capture, "history", dir.filePath("retry"), worker(), selection());
        QVERIFY(load(runNativeAnalysisJob(retry, worker(), 30)).at("ok") == true);
    }
    void cli() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        blendCapture().save(capture);
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"analyze", capture, "history", "--history-scope", "private-draw", "--gpa-event", "100",
                       "--resource", "1000", "--warp", "--output-merger", "--primitive-identity",
                       "--primitive-presence", "--max-steps", "100000", "--out", dir.filePath("cli")});
        QVERIFY(process.waitForFinished(30000));
        QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                 process.readAllStandardError().constData());
        const auto result = load(dir.filePath("cli/result.json"));
        QCOMPARE(result.at("data").at("primitive_identity_observation").at("system_primitive_id"), Json(0));
        QCOMPARE(result.at("data").at("merger_evaluation").at("requested"), Json(true));
        QCOMPARE(result.at("context").at("options").at("max_steps"), Json(100000));
    }
};
QTEST_GUILESS_MAIN(NativeDrawHistoryJobsTests)
#include "NativeDrawHistoryJobsTests.moc"
