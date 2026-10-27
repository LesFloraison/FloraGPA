#include "DepthStencilCapture.h"
#include "HullCapture.h"
#include "MsaaCapture.h"
#include "StreamCapture.h"
#include "UavGeometryCapture.h"
#include "application/Experiment.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeCounters.h"
#include "application/NativeMesh.h"
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
        QCOMPARE(all.at("events").size(), size_t(4));
        for (size_t i = 0; i < 2; ++i) {
            QCOMPARE(all.at("events")[i].at("event"), Json(i ? 810 : 800));
            QCOMPARE(all.at("events")[i].at("boundary"), Json("resource_write_command"));
            for (const auto &counter : all.at("catalog"))
                if (counter.at("type") == "uint64")
                    QCOMPARE(all.at("events")[i].at("values").at(counter.at("id").get<std::string>()),
                             Json(0));
        }
        QCOMPARE(all.at("replay_count").get<unsigned>(), 1u);
        QCOMPARE(all.at("events")[2].at("event").get<Id>(), Id(1000));
        QCOMPARE(all.at("events")[2].at("values").at("input.vertices").get<unsigned>(), 3u);
        QCOMPARE(all.at("events")[3].at("event").get<Id>(), Id(2000));
        QCOMPARE(all.at("events")[3].at("values").at("input.vertices").get<unsigned>(), 0u);
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
    void meshArtifacts_data() {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<unsigned>("vendor");
        for (const auto vendor : {0u, 0x10deu, 0x8086u})
            for (const auto scenario :
                 {"vs", "vs-index", "indexed", "tail", "instanced", "ds", "ds-uav", "ds-uav-independent", "gs", "auto"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(scenario).arg(vendor)))
                    << QString(scenario) << vendor;
    }
    void meshArtifacts() {
        try {
            QFETCH(QString, scenario);
            QFETCH(unsigned, vendor);
            if (vendor) {
                Com<IDXGIFactory1> factory;
                check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Enumerate mesh test adapters");
                bool installed = false;
                for (UINT i = 0;; ++i) {
                    Com<IDXGIAdapter1> adapter;
                    const auto status = factory->EnumAdapters1(i, &adapter);
                    if (status == DXGI_ERROR_NOT_FOUND)
                        break;
                    check(status, "Enumerate mesh test adapter");
                    DXGI_ADAPTER_DESC1 desc{};
                    check(adapter->GetDesc1(&desc), "Describe mesh test adapter");
                    installed |= desc.VendorId == vendor;
                }
                if (!installed)
                    QSKIP("The requested hardware vendor is not installed; WARP is tested separately");
            }
            QTemporaryDir directory;
            const auto capture = directory.filePath("frame.gpa_frame"), output = directory.filePath("mesh");
            auto fixture = scenario == "instanced" || scenario.startsWith("ds") ? testing::hullCapture()
                           : scenario == "gs" || scenario == "auto"             ? testing::streamCapture()
                                                                    : testing::depthStencilCapture();
            if (scenario == "ds-uav")
                fixture = testing::hullUavReadCapture();
            const auto event = scenario == "instanced" || scenario.startsWith("ds") || scenario == "auto"
                                   ? 200u
                               : scenario == "gs" ? 150u
                                                  : 1000u;
            const std::string stage = scenario == "vs"            ? "vs"
                                      : scenario.startsWith("ds") ? "ds"
                                      : scenario == "gs"          ? "gs"
                                                                  : "vs-index";
            if (scenario == "indexed" || scenario == "tail") {
                fixture.entries.erase(
                    std::remove_if(fixture.entries.begin(), fixture.entries.end(),
                                   [](const auto &e) { return e.id == 31 || e.id == 990 || e.id == 1000; }),
                    fixture.entries.end());
                auto code = testing::compileClassProgram(
                    "float4 main(uint id:SV_VertexID):SV_Position{return float4(id,0,0,1);}", "vs_5_0");
                auto bytes = testing::statePack(uint64_t(code.size()));
                bytes.insert(bytes.end(), code.begin(), code.end());
                testing::append(bytes, Id(0));
                fixture.add(31, 9, 0x81, bytes);
                State state{};
                state.topology = 4;
                state.sampleMask = UINT32_MAX;
                state.stages[0].shader = 30;
                state.omStart = 8;
                if (scenario == "indexed") {
                    fixture.add(9000, 5, 0x83,
                                testing::statePack(Id(0), Id(0), 24u, 0u, uint32_t(D3D11_BIND_INDEX_BUFFER),
                                                   0u, 0u, 0u, Id(9001)));
                    fixture.add(9001, 9, 1, testing::statePack(24u, 2u, 0u, 1u, 2u, 1u, 0u));
                    state.ib = 9000;
                    state.ibFormat = DXGI_FORMAT_R32_UINT;
                }
                fixture.add(990, 3, 3, testing::snapshot(state));
                fixture.add(1000, 7, scenario == "indexed" ? 0x39 : 0x37,
                            scenario == "indexed" ? testing::statePack(Id(990), Id(0), Id(1), 6u, 0u, 5u)
                                                  : testing::statePack(Id(990), Id(0), Id(1), 4u, 0u));
            }
            if (scenario == "ds") {
                fixture.entries.erase(std::remove_if(fixture.entries.begin(), fixture.entries.end(),
                                                     [](const auto &e) { return e.id == 7003; }),
                                      fixture.entries.end());
                const auto code = testing::compileClassProgram(
                    "struct V{float4 p:SV_Position;};"
                    "struct C{float edges[3]:SV_TessFactor;float inside:SV_InsideTessFactor;uint tag:ATTR;};"
                    "C patch(InputPatch<V,3> p,uint pid:SV_PrimitiveID){C c;"
                    "c.edges[0]=c.edges[1]=c.edges[2]=c.inside=1;c.tag=42+pid;return c;}"
                    "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")]"
                    "[outputcontrolpoints(3)][patchconstantfunc(\"patch\")]"
                    "V main(InputPatch<V,3> p,uint id:SV_OutputControlPointID){return p[id];}",
                    "hs_5_0");
                auto bytes = testing::statePack(uint64_t(code.size()));
                bytes.insert(bytes.end(), code.begin(), code.end());
                testing::append(bytes, Id(0));
                fixture.add(7003, 9, 0x81, bytes);
            }
            fixture.save(capture);
            Json options{
                {"gpa_event", event}, {"stage", stage}, {"instance", scenario == "instanced" ? 1 : 0}};
            if (vendor)
                options["gpu_vendor"] = vendor;
            else
                options["warp"] = true;
            if (scenario == "ds-uav") {
                const auto unsupported = prepareNativeAnalysisJob(
                    capture, "mesh", directory.filePath("instance"), worker(), options);
                bool rejected = false;
                try {
                    runNativeAnalysisJob(unsupported, worker(), 30);
                } catch (const std::runtime_error &error) {
                    rejected = QString::fromUtf8(error.what())
                                   .contains("Instance boundaries through HS/DS/GS with UAV access");
                }
                QVERIFY2(rejected, "UAV-dependent instance boundaries must not be guessed");
                options.erase("instance");
            }
            const auto job = prepareNativeAnalysisJob(capture, "mesh", output, worker(), options);
            const auto result = load(runNativeAnalysisJob(job, worker(), 30));
            const auto mesh = readNativeMesh(job, result, output, 64 * 1024 * 1024);
            // Both original GS draws emit three vertices into the same SO cursor before DrawAuto.
            const auto expected =
                scenario == "ds-uav" ? 12u
                : scenario == "instanced" || scenario == "ds" || scenario == "ds-uav-independent" || scenario == "auto" || scenario == "indexed"
                    ? 6u
                    : 3u;
            QCOMPARE(mesh.geometry.report.at("vertices").get<unsigned>(), expected);
            if (vendor)
                QCOMPARE(result.at("context").at("adapter").at("vendor").get<unsigned>(), vendor);
            else
                QVERIFY(result.at("context").at("adapter").at("warp") == true);
            if (stage == "vs-index") {
                QCOMPARE(mesh.geometry.identities.size(), size_t(expected));
                for (size_t i = 0; i < mesh.geometry.identities.size(); ++i) {
                    const auto &identity = mesh.geometry.identities[i];
                    const uint32_t index =
                        scenario == "indexed" ? std::array{2u, 0u, 1u, 2u, 1u, 0u}[i] : uint32_t(i);
                    QCOMPARE(identity.instance, scenario == "instanced" ? 1u : 0u);
                    QCOMPARE(identity.vertexId, index);
                    QCOMPARE(identity.vertexIndex, int64_t(index + (scenario == "indexed" ? 5 : 0)));
                }
                if (scenario == "indexed") {
                    QCOMPARE(mesh.tables.at("unique_vertices"), Json(3));
                    const auto &references = mesh.tables.at("tables").at("references").at("rows");
                    QCOMPARE(references[0][8], references[3][8]);
                    QCOMPARE(references[1][8], references[5][8]);
                    QCOMPARE(references[2][8], references[4][8]);
                }
            }
            if (scenario == "tail") {
                // Keep this gap explicit: assembled output omits the fourth input reference.
                QCOMPARE(
                    mesh.geometry.report.at("vertex_identity").at("original_parameters").at("vertex_count"),
                    Json(4));
                QVERIFY(!mesh.geometry.report.at("limits").empty());
            }
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readNativeMesh(job, result, output, 1));
            auto stale = job;
            stale["request_id"] = "another request";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     readNativeMesh(stale, result, output, 64 * 1024 * 1024));
            auto invalid = result;
            invalid["data"]["obj_faces"] = 999;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     readNativeMesh(job, invalid, output, 64 * 1024 * 1024));
            // Corruption that preserves length must fail digest validation.
            QFile file(output + "/vertices.bin");
            QVERIFY(file.open(QIODevice::ReadWrite));
            auto first = file.read(1);
            QVERIFY(!first.isEmpty());
            first[0] = char(uint8_t(first[0]) ^ 0x80);
            QVERIFY(file.seek(0));
            QCOMPARE(file.write(first), qint64(1));
            file.close();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     readNativeMesh(job, result, output, 64 * 1024 * 1024));
            // Verified bytes remain owned by the consumer after exports are damaged.
            QVERIFY(mesh.artifacts.contains("vertices.csv"));
            QCOMPARE(sha256(mesh.geometry.bytes), result.at("data").at("sha256").get<std::string>());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void artifactManifestValidation() {
        QTemporaryDir directory;
        QFile file(directory.filePath("sample.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("abcd"), qint64(4));
        file.close();
        const auto manifest = nativeArtifactManifest(directory.path());
        QCOMPARE(readNativeArtifacts(directory.path(), manifest, 4).value("sample.bin"), QByteArray("abcd"));
        for (const auto name :
             {"../sample.bin", "C:sample.bin", "a\\sample.bin", "sample.bin.", "job.json"}) {
            const Json invalid{{name, manifest.at("sample.bin")}};
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readNativeArtifacts(directory.path(), invalid, 100));
        }
        auto duplicate = manifest;
        duplicate["SAMPLE.BIN"] = manifest.at("sample.bin");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readNativeArtifacts(directory.path(), duplicate, 100));
        auto invalidSize = manifest;
        invalidSize["sample.bin"]["bytes"] = -1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readNativeArtifacts(directory.path(), invalidSize, 100));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 readNativeArtifacts(directory.path(), manifest, 4, [] { return true; }));
        append(directory.filePath("sample.bin"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, verifyNativeArtifacts(directory.path(), manifest));
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
        QCOMPARE(events.size(), size_t(68));
        for (const auto &event : events) {
            const bool draw = event.at("event").get<Id>() >= 1000;
            QCOMPARE(event.at("values").at("input.vertices").get<unsigned>(), draw ? 3u : 0u);
            QCOMPARE(event.at("values").at("input.primitives").get<unsigned>(), draw ? 1u : 0u);
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
        QCOMPARE(measuredReport.at("events").size(), size_t(68));
        QCOMPARE(measured.output().rgba, reference.output().rgba);
        QCOMPARE(measured.counts, reference.counts);
    }
    void writeCounterBoundaries() {
        using namespace testing;
        QTemporaryDir directory;
        const auto capture = directory.filePath("writes.gpa_frame");
        auto fixture = resolvedMsaaOutputCapture(false);
        fixture.buffer(200, 201, 0, 0, {1, 2, 3, 4});
        fixture.buffer(202, 203, 0, 0, {9, 8, 7, 6});
        fixture.add(210, 7, 0x3e, statePack(Id(0), Id(1), Id(202), Id(200)));
        fixture.add(211, 7, 0x40, statePack(Id(0), Id(1), Id(202), 0u, 0u, 0u, 0u, Id(200), 0u, uint8_t(0)));
        fixture.add(212, 7, 0x247, statePack(Id(0), Id(1), Id(202), 0u, uint8_t(0), Id(201), 16u, 16u));
        fixture.save(capture);
        Frame frame(capture.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay reference(frame, options), measured(frame, options);
        reference.run();
        const auto report = collectNativeEventCounters(measured);
        QCOMPARE(measured.readBuffer(202), statePack(1u, 2u, 3u, 4u));
        QCOMPARE(measured.readBuffer(202), reference.readBuffer(202));
        QCOMPARE(measured.readTexture(70), reference.readTexture(70));
        QCOMPARE(measured.counts, reference.counts);
        const auto &rows = report.at("events");
        QCOMPARE(rows.size(), size_t(7));
        const std::array<Id, 7> ids{100, 110, 120, 121, 210, 211, 212};
        for (size_t i = 0; i < ids.size(); ++i) {
            QCOMPARE(rows[i].at("event"), Json(ids[i]));
            QCOMPARE(rows[i].at("timing_valid"), Json(true));
            QVERIFY(rows[i].at("values").at("gpu.duration").get<double>() >= 0);
            if (i < 2) {
                QCOMPARE(rows[i].at("values").at("input.vertices"), Json(3));
                QCOMPARE(rows[i].at("boundary"), Json("submission_after_binding"));
            } else {
                QCOMPARE(rows[i].at("boundary"), Json("resource_write_command"));
                for (const auto &counter : report.at("catalog"))
                    if (counter.at("type") == "uint64")
                        QCOMPARE(rows[i].at("values").at(counter.at("id").get<std::string>()), Json(0));
            }
        }
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
        auto unsupported = prepareNativeAnalysisJob(capture, "debug-pixel", directory.filePath("unsupported"),
                                                    worker(), {{"gpa_event", 1000}});
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
