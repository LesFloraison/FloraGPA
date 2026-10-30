#include "DepthStencilCapture.h"
#include "PixelOrdinalCapture.h"
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
              Json{{"primitive_presence", 1}}, Json{{"primitive_tests", 1}}, Json{{"primitive_tests", true}},
              Json{{"return_count", 1}}, Json{{"max_steps", 5}}}) {
            auto invalid = valid;
            invalid.update(patch);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", invalid));
        }
        auto missing = valid;
        missing.erase("gpa_event");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", missing));
        for (const auto key : {"shader_outputs", "output_merger", "primitive_identity", "primitive_presence",
                               "primitive_tests", "return_count"}) {
            auto events = Json{{"resource", 1000}, {"history_scope", "events"}, {key, false}};
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("history", events));
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("texture", events));
        }
    }
    void isolatedDraw_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode :
                 {"storage", "shader", "merger", "identity", "presence", "tests", "count", "all-msaa"})
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
            if (mode == "count" || msaa)
                options["return_count"] = true;
            if (mode == "shader")
                options["shader_outputs"] = true;
            if (mode == "merger" || msaa)
                options["output_merger"] = true;
            if (mode == "identity" || msaa)
                options["primitive_identity"] = true;
            if (mode == "presence" || mode == "tests" || msaa)
                options["primitive_presence"] = true;
            if (mode == "tests" || msaa)
                options["primitive_tests"] = true;
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
            const auto &count = data.at("return_count_observation");
            QCOMPARE(count.at("requested"), Json(mode == "count" || msaa));
            QCOMPARE(count.at("status"), Json(mode == "count" || msaa ? "observed" : "not_requested"));
            QCOMPARE(count.at("count"), mode == "count" || msaa ? Json(3) : Json(nullptr));
            QVERIFY(count.at("original_fragment_eligibility").is_null());
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
                     Json(mode == "presence" || mode == "tests" || msaa));
            if (mode == "presence" || mode == "tests" || msaa) {
                const auto &presence = data.at("primitive_presence_observation");
                QCOMPARE(presence.at("status"), Json("observed"));
                QCOMPARE(presence.at("observations").size(), size_t(3));
                for (const auto &observed : presence.at("observations")) {
                    QCOMPARE(observed.at("status"), Json("repeat_agreement"));
                    QCOMPARE(observed.at("color_completion_observed"), Json(true));
                    QCOMPARE(observed.at("test_variants").size(), size_t(mode == "tests" || msaa ? 5 : 0));
                    for (const auto &variant : observed.at("test_variants")) {
                        QCOMPARE(variant.at("color_completion_observed"), Json(true));
                        QCOMPARE(variant.at("differs_from_preserved_tests"), Json(false));
                        QVERIFY(variant.at("failure_reason").is_null());
                    }
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
            stale["options"]["return_count"] = !options.value("return_count", false);
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
        auto selected = selection();
        selected["return_count"] = true;
        auto bad = selected;
        bad["x"] = 1;
        const auto failed =
            prepareNativeAnalysisJob(capture, "history", dir.filePath("failed"), worker(), bad);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(failed, worker(), 30));
        QVERIFY(!QFile::exists(dir.filePath("failed/result.json")));
        const auto cancelled =
            prepareNativeAnalysisJob(capture, "history", dir.filePath("cancelled"), worker(), selected);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, runNativeAnalysisJob(cancelled, worker(), 30, [&] {
                                     return QDir(dir.filePath("cancelled")).exists();
                                 }));
        QVERIFY(!QFile::exists(dir.filePath("cancelled/result.json")));
        const auto retry =
            prepareNativeAnalysisJob(capture, "history", dir.filePath("retry"), worker(), selected);
        QVERIFY(load(runNativeAnalysisJob(retry, worker(), 30)).at("ok") == true);
    }
    void integerCount_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("alpha");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t alpha : {1u, 0x3f800000u})
                QTest::newRow(qPrintable(QString("integer-%1-%2").arg(vendor).arg(alpha))) << vendor << alpha;
    }
    void countBoundaries_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"depth", "no-ps", "array"})
                QTest::newRow(qPrintable(QString("boundary-%1-%2").arg(vendor).arg(mode)))
                    << vendor << QString(mode);
    }
    void countBoundaries() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            auto fixture = mode == "depth" ? depthStencilCapture() : blendCapture();
            auto options = selection(vendor);
            options["return_count"] = true;
            if (mode == "depth") {
                options["gpa_event"] = 1000;
                options["resource"] = 720;
            }
            if (mode == "no-ps") {
                std::erase_if(fixture.entries, [](const auto &e) { return e.id == 90 || e.id == 190; });
                State state{};
                state.topology = 4;
                state.sampleMask = UINT32_MAX;
                state.omStart = 8;
                state.rtCount = 1;
                state.rtv[0] = 1002;
                state.blend = 64;
                state.rasterizer = 52;
                state.viewports = 50;
                state.stages[0].shader = 10;
                fixture.add(90, 3, 3, snapshot(state));
                fixture.add(190, 3, 3, snapshot(state));
            }
            if (mode == "array") {
                std::erase_if(fixture.entries, [](const auto &e) { return e.id >= 1000 && e.id <= 1002; });
                fixture.add(1000, 5, 0x85,
                            statePack(Id(0), Id(0), 2u, 2u, 2u, 3u, 2u, 1u, 0u, 0u, 32u, 0u, 0u, Id(0)));
                fixture.add(1002, 5, 0x8d, statePack(Id(0), Id(0), Id(1000), 2u, 5u, 1u, 1u, 2u));
                fixture.add(70, 7, 0x32,
                            statePack(Id(0), Id(1), Id(1002), uint8_t(1), .125f, .5f, .25f, .25f));
                options["mip"] = 1;
                options["layer"] = 1;
            }
            QTemporaryDir dir;
            const auto capture = dir.filePath("frame.gpa_frame");
            fixture.save(capture);
            const auto output = dir.filePath("result");
            const auto job = prepareNativeAnalysisJob(capture, "history", output, worker(), options);
            const auto result = load(runNativeAnalysisJob(job, worker(), 60));
            const auto &count = result.at("data").at("return_count_observation");
            QCOMPARE(count.at("requested"), Json(true));
            QVERIFY(count.at("original_fragment_eligibility").is_null());
            if (mode == "array") {
                QCOMPARE(count.at("count"), Json(1));
                const auto &transport = count.at("observation").at("count_transport");
                QCOMPARE(transport.at("source_mip"), Json(1));
                QCOMPARE(transport.at("source_first_layer"), Json(1));
                QCOMPARE(transport.at("layer"), Json(0));
            } else {
                QCOMPARE(count.at("status"), Json("not_applicable"));
                QVERIFY(count.at("count").is_null());
                QCOMPARE(count.at("reason"),
                         Json(mode == "depth" ? "selected_depth_target" : "no_pixel_shader"));
                QVERIFY(!count.contains("observation"));
            }
            QVERIFY(QFile::copy(capture, output + "/frame.gpa_frame"));
            evidence(output);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void integerCount() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, alpha);
            auto fixture = blendCapture(1, false, true);
            for (const auto &entry : fixture.entries) {
                if (entry.id == 64)
                    put(fixture.bytes, size_t(entry.offset) + 16, 1u);
                if (entry.id == 100)
                    put(fixture.bytes, size_t(entry.offset) + 24, 9u);
            }
            addOrdinalCode(fixture, 11,
                           "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return "
                           "float4(i==2?3:-1,i==1?3:-1,0,1);}",
                           "vs_5_0");
            addOrdinalCode(fixture, 13,
                           "uint4 main():SV_Target{return uint4(7,11,13," + std::to_string(alpha) + "u);}",
                           "ps_5_0");
            QTemporaryDir dir;
            const auto capture = dir.filePath("frame.gpa_frame");
            fixture.save(capture);
            Frame frame(capture.toStdWString());
            ReplayOptions replayOptions;
            replayOptions.until = 100;
            replayOptions.debug = true;
            replayOptions.warp = !vendor;
            if (vendor)
                replayOptions.vendor = vendor;
            Replay reference(frame, replayOptions);
            reference.run();
            const auto original = reference.readTexture(1000);
            const uint32_t expected = Reader(original).read<uint32_t>() == 7 ? 3 : 0;
            auto options = selection(vendor);
            options["return_count"] = true;
            const auto output = dir.filePath("integer");
            const auto job = prepareNativeAnalysisJob(capture, "history", output, worker(), options);
            const auto result = load(runNativeAnalysisJob(job, worker(), 60));
            const auto &data = result.at("data");
            const auto &count = data.at("return_count_observation");
            QCOMPARE(count.at("status"), Json("observed"));
            QCOMPARE(count.at("count"), Json(expected));
            QCOMPARE(count.at("observation").at("count_transport").at("source_slot"), Json(0));
            QCOMPARE(count.at("observation").at("count_transport").at("slot"), Json(1));
            QCOMPARE(data.at("original_draw_observation").at("after").at("bytes"), Json(original));
            QVERIFY(data.at("original_fragment_eligibility").is_null());
            QCOMPARE(data.at("fragment_history_available"), Json(false));
            QCOMPARE(data.at("before"), data.at("restored"));
            save(output + "/verified.json", {{"vendor_request", vendor},
                                             {"alpha_bits", alpha},
                                             {"reference_bytes", original},
                                             {"expected_count", expected}});
            QVERIFY(QFile::copy(capture, output + "/frame.gpa_frame"));
            evidence(output);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void cli() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("frame.gpa_frame");
        blendCapture().save(capture);
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"analyze", capture, "history", "--history-scope", "private-draw", "--gpa-event", "100",
                       "--resource", "1000", "--warp", "--output-merger", "--primitive-identity",
                       "--primitive-presence", "--primitive-tests", "--return-count", "--max-steps", "100000",
                       "--out", dir.filePath("cli")});
        QVERIFY(process.waitForFinished(30000));
        QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                 process.readAllStandardError().constData());
        const auto result = load(dir.filePath("cli/result.json"));
        QCOMPARE(result.at("data").at("primitive_identity_observation").at("system_primitive_id"), Json(0));
        QCOMPARE(result.at("data").at("merger_evaluation").at("requested"), Json(true));
        QCOMPARE(result.at("context").at("options").at("max_steps"), Json(100000));
        QCOMPARE(result.at("data").at("return_count_observation").at("count"), Json(1));
    }
};
QTEST_GUILESS_MAIN(NativeDrawHistoryJobsTests)
#include "NativeDrawHistoryJobsTests.moc"
