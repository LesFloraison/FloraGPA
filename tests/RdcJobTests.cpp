#include "application/RdcJobs.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cstdio>
#include <limits>
#define NOMINMAX
#include <Windows.h>
using Json = nlohmann::json;
using namespace flora;
namespace {
void write(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write job fixture");
}
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read job fixture");
    return f.readAll();
}
Json prepare(const QString &root, const QString &mode, const Json &options = Json::object(),
             const std::string &action = "inventory") {
    const auto capture = root + "/capture & 测试.rdc";
    const auto library = root + "/library.dll";
    write(capture, mode.toUtf8());
    write(library, "library");
    return prepareRdcJob(capture, action, root + "/output & 测试", library, options);
}
int mockWorker(const Json &job) {
    const auto out = QString::fromStdString(job.at("out").get<std::string>());
    const auto mode = read(QString::fromStdString(job.at("capture").get<std::string>()));
    fprintf(stdout, "mock stdout");
    fflush(stdout);
    fprintf(stderr, "mock stderr");
    fflush(stderr);
    if (mode == "timeout") {
        QProcess child;
        child.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--delayed-child", out + "/child-survived.txt"});
        qint64 pid{};
        if (!child.startDetached(&pid))
            return 9;
        write(out + "/child-pid.txt", QByteArray::number(pid));
        QThread::msleep(10000);
    }
    if (mode == "missing")
        return 0;
    if (mode == "bad")
        write(out + "/result.json", "{broken");
    else if (mode == "failed")
        write(out + "/result.json", "{\"ok\":false,\"error\":\"fixture failure\"}");
    else if (mode == "numeric-ok")
        write(out + "/result.json", "{\"ok\":1}");
    else
        write(out + "/result.json",
              "{\"ok\":true,\"annotation\":\"\\udc80\",\"value\":18446744073709551615}");
    return mode == "exit" ? 2 : 0;
}
} // namespace
class RdcJobTests : public QObject {
    Q_OBJECT
  private slots:
    void isolatedPipelinePreparation() {
        QTemporaryDir dir;
        Json options{{"isolate_missing_queries",true},{"execution","9007199254740993"}};
        const auto job=prepare(dir.path(),"success",options,"pipeline");
        QCOMPARE(job.at("execution"),options.at("execution"));
        QCOMPARE(job.at("isolate_missing_queries"),Json(true));
        for (const auto &bad : {Json(nullptr),Json(1),Json("0")}) {
            auto invalid=options; invalid["execution"]=bad;
            QVERIFY_THROWS_EXCEPTION(std::exception,prepare(dir.path(),"success",invalid,"pipeline"));
        }
        auto missing=options; missing.erase("execution");
        QVERIFY_THROWS_EXCEPTION(std::exception,prepare(dir.path(),"success",missing,"pipeline"));
        for (const auto *action : {"postmesh","counters","buffer"})
            QVERIFY_THROWS_EXCEPTION(std::exception,prepare(dir.path(),"success",options,action));
    }
    void isolatedShaderPreparation() {
        QTemporaryDir dir;
        for (const auto *action : {"debug-vertex","debug-pixel","debug-thread"}) {
            Json options{{"isolate_missing_queries",true},{"execution","9007199254740993"}};
            const auto job=prepare(dir.path(),"success",options,action);
            QCOMPARE(job.at("execution"),options.at("execution"));
            QCOMPARE(job.at("isolate_missing_queries"),Json(true));
            for (const auto &bad : {Json(nullptr),Json(1),Json("0")}) {
                auto invalid=options; invalid["execution"]=bad;
                QVERIFY_THROWS_EXCEPTION(std::exception,prepare(dir.path(),"success",invalid,action));
            }
            options.erase("execution");
            QVERIFY_THROWS_EXCEPTION(std::exception,prepare(dir.path(),"success",options,action));
        }
    }
    void replayOptimisationPreparation() {
        QTemporaryDir dir;
        QCOMPARE(rdcReplayOptimisation(Json::object()), 2U);
        uint32_t expected{};
        for (const auto *mode : {"none", "conservative", "balanced", "fastest"}) {
            const auto job = prepare(dir.path(), "success", {{"replay_optimisation", mode}});
            QCOMPARE(rdcReplayOptimisation(job), expected++);
            QCOMPARE(job.at("replay_optimisation"), Json(mode));
        }
        for (const auto &mode : {Json(3), Json(true), Json(nullptr), Json("fast"), Json("")})
            QVERIFY_THROWS_EXCEPTION(std::exception,
                prepare(dir.path(), "success", {{"replay_optimisation", mode}}));
        QCOMPARE(prepare(dir.path(), "success").at("discard_patterns"), Json(false));
        QCOMPARE(prepare(dir.path(), "success", {{"discard_patterns", true}}).at("discard_patterns"), Json(true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success",
            {{"discard_patterns", true}, {"replay_optimisation", "fastest"}}));
        for (const auto &value : {Json(1), Json("false"), Json(nullptr)})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                prepare(dir.path(), "success", {{"discard_patterns", value}}));
    }
    void bufferRangePreparation() {
        QTemporaryDir dir;
        const Json options{{"execution", "14920"}, {"resource", "109459904"}, {"resource_creation", "150566"},
                           {"offset", "9007199254740993"}, {"length", "256"}};
        const auto job = prepare(dir.path(), "success", options, "buffer");
        const auto range = rdcBufferRange(job);
        QCOMPARE(range.offset, (1ULL << 53) + 1);
        QCOMPARE(range.length, 256ULL);
        QCOMPARE(job.at("offset"), options.at("offset"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, "texture"));
        for (const auto &patch : {Json{{"offset", 9007199254740993ULL}}, Json{{"offset", "-1"}},
             Json{{"offset", "01"}}, Json{{"offset", "18446744073709551616"}},
             Json{{"offset", "18446744073709551615"}}, Json{{"length", "0"}},
             Json{{"length", "67108865"}}, Json{{"length", 256}}, Json{{"resource", nullptr}},
             Json{{"isolate_missing_queries", true}}}) {
            auto invalid = options; invalid.update(patch);
            QVERIFY_THROWS_EXCEPTION(std::exception, prepare(dir.path(), "success", invalid, "buffer"));
        }
        auto missing = options; missing.erase("length");
        QVERIFY_THROWS_EXCEPTION(std::exception, prepare(dir.path(), "success", missing, "buffer"));
        auto zero = options; zero["offset"] = "0";
        QCOMPARE(rdcBufferRange(prepare(dir.path(), "success", zero, "buffer")).offset, 0ULL);
    }
    void queryIsolationPreparation() {
        QTemporaryDir dir;
        QCOMPARE(prepare(dir.path(), "success").at("isolate_missing_queries"), Json(false));
        const Json options{{"isolate_missing_queries", true}};
        for (const auto *action : {"inventory", "texture"})
            QCOMPARE(prepare(dir.path(), "success", options, action).at("isolate_missing_queries"), Json(true));
        for (const auto *action : {"resource-history", "history", "debug-pixel", "postmesh", "counters"})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, action));
        auto indexed=options; indexed.update({{"execution","19410"},{"resource","1651480816"},{"resource_creation","35118"}});
        QCOMPARE(prepare(dir.path(),"success",indexed,"resource-history").at("isolate_missing_queries"),Json(true));
        for (const auto key : {"execution","resource","resource_creation"}) {
            auto missing=indexed; missing.erase(key);
            QVERIFY_THROWS_EXCEPTION(std::exception,prepare(dir.path(),"success",missing,"resource-history"));
        }
        for (const auto &value : {Json(1), Json("true"), Json(nullptr)})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                prepare(dir.path(), "success", {{"isolate_missing_queries", value}}));
    }
    void counterSelectionPreparation() {
        QTemporaryDir dir;
        const Json options{{"counter_ids", Json::array({3000001, 1})}, {"execution", "21895"}};
        const auto job = prepare(dir.path(), "success", options, "counters");
        QCOMPARE(job.at("counter_ids"), options.at("counter_ids"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, "inventory"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, "counter-catalog"));
        const auto catalog = prepare(dir.path(), "success", Json::object(), "counter-catalog");
        QCOMPARE(catalog.at("action"), Json("counter-catalog"));
        QVERIFY(!catalog.contains("counter_ids"));
        for (const auto &ids : {Json::array(), Json::array({0}), Json::array({1, 1}), Json::array({-1}),
             Json::array({1.5}), Json::array({uint64_t(UINT32_MAX) + 1}), Json(nullptr)})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                prepare(dir.path(), "success", Json{{"counter_ids", ids}}, "counters"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            prepare(dir.path(), "success", Json{{"execution", "1"}}, "counter-catalog"));
    }
    void fastContractionPreparation() {
        QTemporaryDir dir;
        const Json options{{"execution", "37003"}, {"dxil_fast_contraction", true}};
        const auto job = prepare(dir.path(), "success", options, "debug-pixel");
        QCOMPARE(job.at("dxil_fast_contraction"), Json(true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, "postmesh"));
        auto invalid = options; invalid.erase("execution"); invalid["gpa_event"] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "debug-pixel"));
        invalid = options; invalid["dxil_fast_contraction"] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "debug-pixel"));
    }
    void inputAttributePreparation() {
        QTemporaryDir dir;
        const Json options{{"execution", "36560"}, {"stage", "VSIn"}, {"attribute", 1}};
        const auto job = prepare(dir.path(), "success", options, "postmesh");
        QCOMPARE(job.at("attribute"), Json(1)); QCOMPARE(job.at("stage"), Json("VSIn"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, "debug-vertex"));
        auto invalid = options; invalid.erase("execution"); invalid["gpa_event"] = 36560;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "postmesh"));
        invalid = options; invalid["stage"] = "VSOut";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "postmesh"));
        for (const auto value : {Json(-1), Json(0.5), Json(uint64_t(UINT32_MAX) + 1)}) {
            invalid = options; invalid["attribute"] = value;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "postmesh"));
        }
    }
    void indirectActionPreparation() {
        QTemporaryDir dir;
        for (const auto &invalid : {Json{{"indirect_action", 0}},
                Json{{"execution", "9007199254740993"}, {"indirect_action", -1}},
                Json{{"execution", "9007199254740993"}, {"indirect_action", 0.5}},
                Json{{"execution", "9007199254740993"}, {"indirect_action", uint64_t(UINT32_MAX) + 1}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "debug-thread"));
        const auto job = prepare(dir.path(), "success", {{"execution", "9007199254740993"}, {"indirect_action", 1}}, "debug-thread");
        QCOMPARE(job.at("execution"), Json("9007199254740993")); QCOMPARE(job.at("indirect_action"), Json(1));
    }
    void dx12Preparation() {
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), "success",
                                 {{"execution", "9007199254740993"},
                                  {"resource", "18446744073709551615"},
                                  {"resource_creation", "9007199254740994"}, {"primitive", 17},
                                  {"api_validation", true}, {"readonly_depth_snapshot", true}}, "texture");
        QCOMPARE(job.at("execution"), Json("9007199254740993"));
        QCOMPARE(job.at("resource"), Json("18446744073709551615"));
        QVERIFY(job.at("gpa_event").is_null());
        QCOMPARE(job.at("primitive"), Json(17));
        QCOMPARE(job.at("api_validation"), Json(true));
        QCOMPARE(job.at("readonly_depth_snapshot"), Json(true));
    }
    void preparation() {
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), "success", {{"resource", UINT64_MAX}, {"group", {1, 2, 3}}});
        QCOMPARE(job.at("resource"), Json(UINT64_MAX));
        QCOMPARE(job.at("index"), Json(nullptr));
        QCOMPARE(job.at("stage"), Json("VSOut"));
        QCOMPARE(job.at("api_validation"), Json(false));
        QCOMPARE(job.at("readonly_depth_snapshot"), Json(false));
        QCOMPARE(job.at("depth_roundtrip"), Json(false));
        QCOMPARE(job.at("pixel_history_reference"), Json(false));
        QCOMPARE(job.at("group"), Json::array({1, 2, 3}));
        QCOMPARE(job.at("thread"), Json::array({0, 0, 0}));
        QVERIFY(QFileInfo(QString::fromStdString(job.at("capture").get<std::string>())).isAbsolute());
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        QVERIFY(QDir(out).entryList(QDir::Files | QDir::Hidden).isEmpty());
        write(out + "/.keep", "preserve");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success"));
        QCOMPARE(read(out + "/.keep"), QByteArray("preserve"));
    }
    void pixelHistoryReference() {
        QTemporaryDir dir;
        for (const auto &invalid : {Json{{"pixel_history_reference", "true"}},
                                    Json{{"pixel_history_reference", true}},
                                    Json{{"pixel_history_reference", true}, {"primitive", UINT32_MAX}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "debug-pixel"));
        const Json options{{"pixel_history_reference", true}, {"primitive", 1322}, {"execution", "36560"}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options, "texture"));
        const auto job = prepare(dir.path(), "success", options, "debug-pixel");
        QCOMPARE(job.at("pixel_history_reference"), Json(true));
        QCOMPARE(job.at("primitive"), Json(1322));
    }
    void depthRoundtripScope() {
        QTemporaryDir dir;
        const Json option{{"execution", "36222"}, {"depth_roundtrip", true}};
        for (const auto *action : {"debug-vertex", "debug-thread", "counters"})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", option, action));
        auto conflict = option;
        conflict["readonly_depth_snapshot"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", conflict, "texture"));
        auto invalid = option;
        invalid["depth_roundtrip"] = "true";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", invalid, "texture"));
        const auto job = prepare(dir.path(), "success", option, "texture");
        QCOMPARE(job.at("depth_roundtrip"), Json(true));
        QCOMPARE(job.at("execution"), Json("36222"));
        const auto pixel = prepare(dir.path(), "success", option, "debug-pixel");
        QCOMPARE(pixel.at("depth_roundtrip"), Json(true));
        const auto history = prepare(dir.path(), "success", option, "history");
        QCOMPARE(history.at("depth_roundtrip"), Json(true));
    }
    void invalidOptions_data() {
        QTest::addColumn<Json>("options");
        QTest::newRow("unknown") << Json{{"invented", 1}};
        QTest::newRow("validation-type") << Json{{"api_validation", "true"}};
        QTest::newRow("depth-snapshot-type") << Json{{"readonly_depth_snapshot", "true"}};
        QTest::newRow("depth-snapshot-action") << Json{{"readonly_depth_snapshot", true}};
        QTest::newRow("boolean") << Json{{"x", true}};
        QTest::newRow("negative") << Json{{"y", -1}};
        QTest::newRow("overflow") << Json{{"sample", uint64_t(UINT32_MAX) + 1}};
        QTest::newRow("primitive-overflow") << Json{{"primitive", uint64_t(UINT32_MAX) + 1}};
        QTest::newRow("primitive-negative") << Json{{"primitive", -1}};
        QTest::newRow("fraction") << Json{{"vertex", 1.5}};
        QTest::newRow("both-events") << Json{{"gpa_event", 1}, {"eid", 2}};
        QTest::newRow("dx12-number") << Json{{"execution", 9007199254740993ULL}};
        QTest::newRow("dx12-overflow") << Json{{"execution", "18446744073709551616"}};
        QTest::newRow("dx12-null") << Json{{"execution", nullptr}};
        QTest::newRow("dx12-mixed-event") << Json{{"execution", "1"}, {"gpa_event", 1}};
        QTest::newRow("dx12-mixed-resource") << Json{{"execution", "1"}, {"resource", 1}};
        QTest::newRow("dx12-lifetime-without-object") << Json{{"execution", "1"}, {"resource_creation", "2"}};
        QTest::newRow("short-group") << Json{{"group", {0, 0}}};
        QTest::newRow("thread-type") << Json{{"thread", "000"}};
        QTest::newRow("negative-thread") << Json{{"thread", {0, 0, -1}}};
        QTest::newRow("stage") << Json{{"stage", "VSIn"}};
        QTest::newRow("options-array") << Json::array();
    }
    void invalidOptions() {
        QFETCH(Json, options);
        QTemporaryDir dir;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options));
        QVERIFY(!QFileInfo::exists(dir.path() + "/output & 测试"));
    }
    void workerResult_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("message");
        QTest::newRow("success-surrogates") << "success" << "";
        QTest::newRow("missing-result") << "missing" << "did not produce a result";
        QTest::newRow("malformed-result") << "bad" << "Invalid analysis result";
        QTest::newRow("false-result") << "failed" << "fixture failure";
        QTest::newRow("numeric-success") << "numeric-ok" << "analysis failed";
        QTest::newRow("failed-exit") << "exit" << "worker failed";
    }
    void workerResult() {
        QFETCH(QString, mode);
        QFETCH(QString, message);
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), mode);
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        QString error;
        try {
            QCOMPARE(runRdcAnalysis(job, QCoreApplication::applicationFilePath(), 10), out + "/result.json");
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        if (message.isEmpty()) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(read(out + "/result.json"),
                     QByteArray("{\"ok\":true,\"annotation\":\"\\udc80\",\"value\":18446744073709551615}"));
        } else
            QVERIFY2(error.contains(message), qPrintable(error));
        QCOMPARE(Json::parse(read(out + "/job.json").toStdString()), job);
        QCOMPARE(read(out + "/worker.log"), QByteArray("mock stdout\nmock stderr"));
    }
    void timeoutKillsChildren() {
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), "timeout");
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        QElapsedTimer timer;
        timer.start();
        QString error;
        try {
            runRdcAnalysis(job, QCoreApplication::applicationFilePath(), 0.75);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QVERIFY2(error.contains("timed out"), qPrintable(error));
        QVERIFY(timer.elapsed() < 5000);
        const auto pid = read(out + "/child-pid.txt").toUInt();
        const auto child = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (child) {
            const auto status = WaitForSingleObject(child, 2000);
            CloseHandle(child);
            QCOMPARE(status, DWORD(WAIT_OBJECT_0));
        }
        QVERIFY(!QFileInfo::exists(out + "/child-survived.txt"));
        QVERIFY(read(out + "/worker.log").contains("mock stdout"));
        QCOMPARE(Json::parse(read(out + "/job.json").toStdString()), job);
    }
    void invalidTimeoutAndStart() {
        QTemporaryDir dir;
        auto job = prepare(dir.path(), "success");
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        for (const auto seconds : {0., -1., std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN(), 1e20})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     runRdcAnalysis(job, QCoreApplication::applicationFilePath(), seconds));
        QVERIFY(QDir(out).entryList(QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runRdcAnalysis(job, dir.path() + "/library.dll", 10));
        QVERIFY(QFileInfo::exists(out + "/worker.log"));
        QCOMPARE(Json::parse(read(out + "/job.json").toStdString()), job);
    }
    void publicCliArguments() {
        QTemporaryDir dir;
        const auto capture = dir.path() + "/capture & 测试.rdc";
        write(capture, "invalid capture: the library check must run first");
        const auto library = qEnvironmentVariable("WINDIR") + "/System32/version.dll";
        const auto out = dir.path() + "/CLI 结果";
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", {"rdc-analyze",
                                                                                     capture,
                                                                                     "inventory",
                                                                                     "--out",
                                                                                     out,
                                                                                     "--renderdoc",
                                                                                     library,
                                                                                     "--gpa-event",
                                                                                     "18446744073709551615",
                                                                                     "--resource",
                                                                                     "18446744073709551615",
                                                                                     "--sample",
                                                                                     "4294967295",
                                                                                     "--group",
                                                                                     "1",
                                                                                     "2",
                                                                                     "3",
                                                                                     "--thread",
                                                                                     "4",
                                                                                     "5",
                                                                                     "6"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains("Missing RenderDoc entry point"));
        const auto job = Json::parse(read(out + "/job.json").toStdString());
        QCOMPARE(job.at("gpa_event"), Json(UINT64_MAX));
        QCOMPARE(job.at("resource"), Json(UINT64_MAX));
        QCOMPARE(job.at("sample"), Json(UINT32_MAX));
        QCOMPARE(job.at("group"), Json::array({1, 2, 3}));
        QCOMPARE(job.at("thread"), Json::array({4, 5, 6}));
        QCOMPARE(job.at("index"), Json(nullptr));
        QVERIFY(QFileInfo::exists(out + "/worker.log"));
        const auto originalJob = read(out + "/job.json");
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"rdc-analyze", capture, "inventory", "--out", out, "--renderdoc", library});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains("new or empty"));
        QCOMPARE(read(out + "/job.json"), originalJob);
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"rdc-analyze", "--help"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains("FloraGPA.Cli rdc-analyze"));
    }
    void publicCliRejectsInvalidOptions_data() {
        QTest::addColumn<QStringList>("options");
        QTest::newRow("both-events") << QStringList{"--gpa-event", "1", "--eid", "2"};
        QTest::newRow("group-size") << QStringList{"--group", "0", "0"};
        QTest::newRow("thread-negative") << QStringList{"--thread", "0", "-1", "0"};
        QTest::newRow("overflow") << QStringList{"--mip", "4294967296"};
        QTest::newRow("negative-index") << QStringList{"--index", "-1"};
        QTest::newRow("fraction") << QStringList{"--x", "1.5"};
        QTest::newRow("stage") << QStringList{"--stage", "DSOut"};
        QTest::newRow("option") << QStringList{"--invented", "1"};
        QTest::newRow("timeout-zero") << QStringList{"--timeout", "0"};
        QTest::newRow("timeout-infinite") << QStringList{"--timeout", "inf"};
    }
    void publicCliRejectsInvalidOptions() {
        QFETCH(QStringList, options);
        QTemporaryDir dir;
        const auto capture = dir.path() + "/capture.rdc";
        write(capture, "unused");
        const auto out = dir.path() + "/output";
        QStringList args{"rdc-analyze",
                         capture,
                         "inventory",
                         "--out",
                         out,
                         "--renderdoc",
                         qEnvironmentVariable("WINDIR") + "/System32/version.dll"};
        args.append(options);
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(!process.readAllStandardError().isEmpty());
        QVERIFY(!QFileInfo::exists(out + "/result.json"));
        QVERIFY(!QFileInfo::exists(out + "/job.json"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 3 && args[1] == "--job")
        return mockWorker(Json::parse(read(args[2]).toStdString()));
    if (args.size() == 3 && args[1] == "--delayed-child") {
        QThread::msleep(3000);
        write(args[2], "child survived");
        return 0;
    }
    RdcJobTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "RdcJobTests.moc"
