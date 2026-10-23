#include "DepthStencilCapture.h"
#include "application/Experiment.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeCounters.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

using namespace flora;
using Json = nlohmann::json;
namespace {
QString worker() { return QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe"; }
Json load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read test report");
    return Json::parse(file.readAll().toStdString());
}
void append(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::Append))
        throw std::runtime_error("Cannot mutate test input");
    file.write(" ");
}
void verifyChildStopped(const QString &directory) {
    QFile file(directory + "/child.pid");
    QVERIFY(file.open(QIODevice::ReadOnly));
    bool valid = false;
    const auto pid = file.readAll().toUInt(&valid);
    QVERIFY(valid);
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        QCOMPARE(GetLastError(), DWORD(ERROR_INVALID_PARAMETER));
        return;
    }
    const auto status = WaitForSingleObject(process, 2000);
    CloseHandle(process);
    QCOMPARE(status, DWORD(WAIT_OBJECT_0));
}
} // namespace
class NativeAnalysisTests final : public QObject {
    Q_OBJECT
  private slots:
    void requestValidation() {
        for (const auto &options : {Json{{"eid", 10}}, Json{{"renderdoc", "library"}}, Json{{"resource", -1}},
                                    Json{{"resource", 1}, {"sample", uint64_t(UINT32_MAX) + 1}},
                                    Json{{"resource", 1}, {"warp", true}, {"gpu_vendor", 0x8086}}})
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("texture", options));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, nativeAnalysisRequest("mesh", Json::object()));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                 nativeAnalysisRequest("counters", {{"start_event", 4}}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                 nativeAnalysisRequest("counters", {{"start_event", 4}, {"end_event", 3}}));
        QVERIFY_THROWS_EXCEPTION(
            std::invalid_argument,
            nativeAnalysisRequest("debug-thread", {{"gpa_event", 4}, {"thread", {1, 2}}}));
        QCOMPARE(nativeAnalysisRequest("debug-thread", {{"gpa_event", 4}, {"thread", {1, 2, 3}}}).action,
                 std::string("debug-thread"));
    }
    void isolatedInventoryAndStaleResult() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::depthStencilCapture().save(capture);
        const auto job =
            prepareNativeAnalysisJob(capture, "inventory", directory.filePath("output"), worker());
        const auto result = load(runNativeAnalysisJob(job, worker(), 30));
        QVERIFY(result.at("context").at("adapter").is_null());
        QVERIFY(!result.at("data").at("resources").empty());
        QVERIFY(result.at("context").at("frame_sha256") == analysisFileDigest(capture));
        for (const auto &module : result.at("execution").at("loaded_modules")) {
            const auto name = QString::fromStdString(module.get<std::string>()).toLower();
            QVERIFY(!name.contains("renderdoc"));
            QVERIFY(!name.endsWith("dx11_player.dll"));
            QVERIFY(!name.endsWith("dx11_playback.dll"));
        }
        for (const auto key :
             {"request_id", "frame_sha256", "backend_sha256", "experiment_sha256", "action", "options"}) {
            auto stale = job;
            stale[key] = "changed";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateNativeAnalysisResult(stale, result));
        }
        auto tampered = result;
        tampered["context"]["adapter"] = "changed";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateNativeAnalysisResult(job, tampered));
        auto wrongAdapter = job;
        wrongAdapter["expected_adapter"] = {{"vendor", 123}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validateNativeAnalysisResult(wrongAdapter, result));
    }
    void workerCountersAndExperiment_data() {
        QTest::addColumn<unsigned>("vendor");
        QTest::newRow("WARP") << 0u;
        QTest::newRow("NVIDIA") << 0x10deu;
        QTest::newRow("Intel") << 0x8086u;
    }
    void workerCountersAndExperiment() {
        QFETCH(unsigned, vendor);
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Enumerate test adapters");
            bool installed = false;
            for (UINT index = 0;; ++index) {
                Com<IDXGIAdapter1> adapter;
                const auto status = factory->EnumAdapters1(index, &adapter);
                if (status == DXGI_ERROR_NOT_FOUND)
                    break;
                check(status, "Enumerate test adapter");
                DXGI_ADAPTER_DESC1 description{};
                check(adapter->GetDesc1(&description), "Describe test adapter");
                installed |= description.VendorId == vendor;
            }
            if (!installed)
                QSKIP("The requested hardware vendor is not installed; WARP is tested separately");
        }
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::depthStencilCapture().save(capture);
        Frame frame(capture.toStdWString());
        Experiment experiment(frame);
        experiment.setEnabled(frame, 2000, false);
        const auto project = directory.filePath("experiment.json");
        experiment.save(project);
        Json options{{"start_event", 800}, {"end_event", 2000}};
        if (vendor)
            options["gpu_vendor"] = vendor;
        else
            options["warp"] = true;
        const auto job = prepareNativeAnalysisJob(capture, "counters", directory.filePath("output"), worker(),
                                                  options, project);
        const auto result = load(runNativeAnalysisJob(job, worker(), 30));
        QCOMPARE(result.at("data").at("pipeline").at("ia_vertices").get<unsigned>(), 3u);
        QVERIFY(result.at("context").at("adapter").at("warp") == (vendor == 0));
        if (vendor)
            QCOMPARE(result.at("context").at("adapter").at("vendor").get<unsigned>(), vendor);
        QVERIFY(result.at("data").at("timing").at("available") == true);
        QVERIFY(result.at("data").at("timing").at("elapsed_ms").get<double>() >= 0);
        Json allOptions = vendor ? Json{{"gpu_vendor", vendor}} : Json{{"warp", true}};
        const auto allJob = prepareNativeAnalysisJob(capture, "counters", directory.filePath("all"), worker(),
                                                     allOptions, project);
        const auto all = load(runNativeAnalysisJob(allJob, worker(), 30)).at("data");
        QCOMPARE(all.at("catalog").size(), size_t(13));
        QCOMPARE(all.at("events").size(), size_t(2));
        QCOMPARE(all.at("replay_count").get<unsigned>(), 1u);
        QCOMPARE(all.at("events")[0].at("event").get<Id>(), Id(1000));
        QCOMPARE(all.at("events")[0].at("values").at("input.vertices").get<unsigned>(), 3u);
        QCOMPARE(all.at("events")[1].at("event").get<Id>(), Id(2000));
        QCOMPARE(all.at("events")[1].at("values").at("input.vertices").get<unsigned>(), 0u);
        auto changed = prepareNativeAnalysisJob(capture, "counters", directory.filePath("changed"), worker(),
                                                options, project);
        append(project);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(changed, worker(), 30));
        QVERIFY(!QFileInfo::exists(directory.filePath("changed/result.json")));
    }
    void resourcesAndGeometry() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        auto fixture = testing::depthStencilCapture();
        for (const auto &entry : fixture.entries)
            if (entry.id == 20)
                testing::put(fixture.bytes, size_t(entry.offset + entry.size - sizeof(Id)), Id(7000));
        fixture.add(7000, 9, 1, testing::statePack(4u, 0xff00ff00u));
        fixture.save(capture);
        for (const auto &action : {std::string("events"), std::string("texture"), std::string("mesh")}) {
            Json options = Json::object();
            if (action == "texture")
                options = {{"resource", 20}, {"gpa_event", 2000}, {"warp", true}};
            if (action == "mesh")
                options = {{"gpa_event", 1000}, {"stage", "vs"}, {"warp", true}};
            const auto output = directory.filePath(QString::fromStdString(action));
            const auto job = prepareNativeAnalysisJob(capture, action, output, worker(), options);
            const auto result = load(runNativeAnalysisJob(job, worker(), 30));
            QVERIFY(result.at("ok") == true);
            QVERIFY(!result.at("data").is_null());
            if (action == "events")
                QVERIFY(QFileInfo::exists(output + "/commands.csv"));
            if (action == "texture") {
                QCOMPARE(result.at("data").at("resource_id").get<Id>(), Id(20));
                QVERIFY(result.at("data").at("value_time") == "after_event");
            }
            if (action == "mesh")
                QCOMPARE(result.at("data").at("vertices").get<unsigned>(), 3u);
        }
        const auto initialJob = prepareNativeAnalysisJob(capture, "texture", directory.filePath("initial"),
                                                         worker(), {{"resource", 20}, {"warp", true}});
        const auto initial = load(runNativeAnalysisJob(initialJob, worker(), 30));
        QVERIFY(initial.at("data").at("value_time") == "capture_initial");
        QVERIFY(initial.at("data").at("sha256") !=
                load(directory.filePath("texture/result.json")).at("data").at("sha256"));
        QFile initialBytes(directory.filePath("initial/subresource.bin"));
        QFile finalBytes(directory.filePath("texture/subresource.bin"));
        QVERIFY(initialBytes.open(QIODevice::ReadOnly));
        QVERIFY(finalBytes.open(QIODevice::ReadOnly));
        QCOMPARE(initialBytes.readAll(), QByteArray::fromHex("00ff00ff"));
        QCOMPARE(finalBytes.readAll(), QByteArray::fromHex("ff0000ff"));
    }
    void counterBatchBoundary() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        auto fixture = testing::depthStencilCapture();
        for (Id index = 0; index < 64; ++index)
            fixture.add(2100 + index, 7, 0x37, testing::statePack(Id(1990), Id(0), Id(1), 3u, 0u));
        fixture.save(capture);
        const auto job = prepareNativeAnalysisJob(capture, "counters", directory.filePath("output"), worker(),
                                                  {{"warp", true}});
        const auto result = load(runNativeAnalysisJob(job, worker(), 30));
        const auto &events = result.at("data").at("events");
        QCOMPARE(events.size(), size_t(66));
        for (const auto &event : events) {
            QCOMPARE(event.at("values").at("input.vertices").get<unsigned>(), 3u);
            QCOMPARE(event.at("values").at("input.primitives").get<unsigned>(), 1u);
        }
        for (const auto &module : result.at("execution").at("loaded_modules")) {
            const auto name = QString::fromStdString(module.get<std::string>()).toLower();
            QVERIFY(!name.contains("renderdoc"));
            QVERIFY(!name.contains("intelswtools\\gpa\\"));
        }
        Frame frame(capture.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay reference(frame, options), measured(frame, options);
        reference.run();
        const auto measuredReport = collectNativeEventCounters(measured);
        QCOMPARE(measuredReport.at("events").size(), size_t(66));
        QCOMPARE(measured.output().rgba, reference.output().rgba);
        QCOMPARE(measured.counts, reference.counts);
    }
    void failCancelTimeoutAndRetry() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::depthStencilCapture().save(capture);
        const auto self = QCoreApplication::applicationFilePath();
        auto job = prepareNativeAnalysisJob(capture, "inventory", directory.filePath("timeout"), self);
        QElapsedTimer timer;
        timer.start();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(job, self, 1));
        QVERIFY(timer.elapsed() < 12000);
        verifyChildStopped(directory.filePath("timeout"));
        job["out"] = directory.filePath("cancel").toStdString();
        timer.restart();
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 runNativeAnalysisJob(job, self, 30, [&] { return timer.elapsed() > 1000; }));
        QVERIFY(timer.elapsed() < 12000);
        verifyChildStopped(directory.filePath("cancel"));
        job["out"] = directory.filePath("crash").toStdString();
        job["test_crash"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(job, self, 30));
        const auto retry =
            prepareNativeAnalysisJob(capture, "inventory", directory.filePath("retry"), worker());
        QVERIFY(load(runNativeAnalysisJob(retry, worker(), 30)).at("ok") == true);
        auto unsupported = prepareNativeAnalysisJob(
            capture, "debug-vertex", directory.filePath("unsupported"), worker(), {{"gpa_event", 1000}});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(unsupported, worker(), 30));
        QVERIFY(!QFileInfo::exists(directory.filePath("unsupported/result.json")));
        auto changed =
            prepareNativeAnalysisJob(capture, "inventory", directory.filePath("changed"), worker());
        append(capture);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runNativeAnalysisJob(changed, worker(), 30));
    }
    void cliEntry() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::depthStencilCapture().save(capture);
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"analyze", capture, "inventory", "--out", directory.filePath("cli")});
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(load(directory.filePath("cli/result.json")).at("ok") == true);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 2 && app.arguments()[1] == "--analysis-test-child") {
        QThread::sleep(60);
        return 0;
    }
    if (app.arguments().size() == 5 && app.arguments()[1] == "--native-analysis-job") {
        while (!QFileInfo::exists(app.arguments()[4]))
            QThread::msleep(10);
        const auto job = load(app.arguments()[2]);
        if (job.value("test_crash", false)) {
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
            TerminateProcess(GetCurrentProcess(), 123);
        }
        QProcess child;
        child.start(app.applicationFilePath(), {"--analysis-test-child"});
        if (!child.waitForStarted(10000))
            return 3;
        QFile pid(QString::fromStdString(job.at("out")) + "/child.pid");
        if (!pid.open(QIODevice::WriteOnly))
            return 4;
        pid.write(QByteArray::number(child.processId()));
        pid.close();
        QThread::sleep(60);
        return 0;
    }
    NativeAnalysisTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "NativeAnalysisTests.moc"
