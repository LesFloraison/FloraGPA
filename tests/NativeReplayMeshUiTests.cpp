#include "DepthStencilCapture.h"
#include "HullCapture.h"
#include "StreamCapture.h"
#include "UavGeometryCapture.h"
#include "app/MainWindow.h"
#include "app/ReplayMeshView.h"
#include "application/NativeAnalysisJobs.h"
#include "application/NativeReplayMesh.h"
#include "application/PostTransform.h"
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read native mesh test output");
    return file.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot save native mesh test output");
}
} // namespace
class NativeReplayMeshUiTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPATests");
        QCoreApplication::setApplicationName("NativeReplayMeshTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings().clear();
        QSettings().setValue("analysis/renderdoc", "C:/missing/retired-library.dll");
    }
    void pipeline_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("stage");
        for (auto vendor : {0u, 4318u, 32902u})
            for (const auto stage : {"vs", "gs", "ds", "ds-uav", "ds-uav-independent"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(stage)))
                    << vendor << QString(stage);
    }
    void positionExports_data() {
        QTest::addColumn<uint32_t>("components");
        QTest::addColumn<bool>("nonfinite");
        for (uint32_t count = 1; count <= 4; ++count)
            for (bool special : {false, true})
                QTest::newRow(qPrintable(QString("components-%1-special-%2").arg(count).arg(special)))
                    << count << special;
    }
    void positionExports() {
        QFETCH(uint32_t, components);
        QFETCH(bool, nonfinite);
        PostTransformGeometry geometry;
        Json channels = Json::array();
        for (uint32_t i = 0; i < components; ++i)
            channels.push_back(i);
        geometry.report = {{"requested_stage", "vs"},
                           {"shader_stage", "vs"},
                           {"event", {{"id", 1}}},
                           {"stream", 0},
                           {"vertices", 3},
                           {"primitives", 1},
                           {"vertices_per_primitive", 3},
                           {"stride", 20},
                           {"attributes",
                            {{{"system_value", 1},
                              {"component_type", 3},
                              {"components", channels},
                              {"component_count", components},
                              {"offset", 4},
                              {"semantic", "SV_Position"},
                              {"index", 0}}}}};
        const std::array<std::array<float, 4>, 3> positions{
            {{2, 4, 6, 2},
             {-0.f, 1, 2, 0},
             {nonfinite ? std::numeric_limits<float>::infinity() : 3.f, 2, 1, 1}}};
        geometry.bytes.resize(60);
        for (size_t row = 0; row < 3; ++row)
            std::memcpy(geometry.bytes.data() + row * 20 + 4, positions[row].data(), 16);
        geometry.report["sha256"] = sha256(geometry.bytes);
        const auto tables = postTransformTables(geometry);
        for (const auto key : {"vertex_references", "obj_vertices", "obj_faces", "obj_lines", "obj_points",
                               "obj_unavailable_reason"})
            geometry.report[key] = tables.at(key);
        Json job{{"schema", nativeAnalysisSchema},
                 {"backend", nativeAnalysisBackend},
                 {"backend_sha256", "owned-fixture"},
                 {"frame_sha256", "owned-fixture"},
                 {"experiment_sha256", nullptr},
                 {"action", "mesh"},
                 {"options", {{"gpa_event", 1}, {"stage", "vs"}}},
                 {"request_id", "owned-fixture"}};
        auto context = job;
        context.erase("request_id");
        Json result{{"schema", nativeAnalysisSchema},
                    {"backend", nativeAnalysisBackend},
                    {"ok", true},
                    {"request_id", job.at("request_id")},
                    {"context", context},
                    {"context_id", analysisIdentity(context)},
                    {"data", geometry.report}};
        auto model = nativeReplayMesh(job, result, postTransformFiles(geometry));
        QCOMPARE(model.positions.components, components);
        QCOMPARE(model.positions.faces, (std::vector<std::array<uint64_t, 3>>{{0, 1, 2}}));
        for (size_t row = 0; row < 3; ++row)
            for (uint32_t c = 0; c < components; ++c)
                QCOMPARE(model.positions.positions[row][c], double(positions[row][c]));
        QVERIFY(model.files.value("post_vertices.csv").contains("-0.0"));
        QVERIFY(model.files.value("post_geometry.obj").contains("f 1 2 3\r\n"));
        QCOMPARE(model.preview.empty(), nonfinite || components < 3);
        if (components == 4 && !nonfinite)
            QVERIFY(model.files.value("post_geometry.obj").contains("v 1.0 2.0 3.0\r\n"));
        if (components >= 3 && !nonfinite)
            QVERIFY(model.files.value("post_geometry.obj").contains("v -0.0 1.0 2.0\r\n"));
    }
    void pipeline() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, stage);
            QTemporaryDir directory;
            const auto capture = directory.filePath("mesh.gpa_frame"), output = directory.filePath("result");
            auto fixture = stage == "vs"   ? testing::depthStencilCapture()
                           : stage == "gs" ? testing::streamCapture()
                                           : testing::hullCapture();
            if (stage == "ds-uav")
                fixture = testing::hullUavReadCapture();
            if (stage == "ds") {
                fixture.entries.erase(std::remove_if(fixture.entries.begin(), fixture.entries.end(),
                                                     [](const auto &entry) { return entry.id == 7003; }),
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
            const Id event = stage == "vs" ? 1000 : stage == "gs" ? 150 : 200;
            ReplayMeshView view;
            view.setNativeMode(true);
            view.setContext("test", event);
            view.findChild<QComboBox *>("replayMeshStage")->setCurrentIndex(stage == "vs" ? 0 : 1);
            if (stage.startsWith("ds"))
                view.findChild<QLineEdit *>("replayMeshInstance")->setText("1");
            auto options = view.request();
            QCOMPARE(options.at("action"), Json("mesh"));
            options.erase("action");
            if (vendor)
                options["gpu_vendor"] = vendor;
            else
                options["warp"] = true;
            const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
            const auto job = prepareNativeAnalysisJob(capture, "mesh", output, worker, options);
            if (stage == "ds-uav") {
                bool rejected = false;
                try {
                    runNativeAnalysisJob(job, worker, 60);
                } catch (const std::runtime_error &error) {
                    rejected = QString::fromUtf8(error.what())
                                   .contains("Instance boundaries through HS/DS/GS with UAV access");
                }
                QVERIFY2(rejected,
                         "The unresolved instance-identity gate must fail explicitly without fallback");
                QVERIFY(view.result().is_null());
                const auto retained = qEnvironmentVariable("FLORA_NATIVE_MESH_ARTIFACT_DIR");
                if (!retained.isEmpty()) {
                    const auto folder = retained + '/' + QString::fromLatin1(QTest::currentDataTag());
                    QVERIFY(QDir().mkpath(folder));
                    write(folder + "/job.json", QByteArray::fromStdString(job.dump(2)));
                    write(folder + "/worker.log", read(output + "/worker.log"));
                    write(folder + "/open-gate.json",
                          "{\"instance_selection_supported\":false,\"fallback\":false}\n");
                }
                return;
            }
            const auto result = Json::parse(read(runNativeAnalysisJob(job, worker, 60)).toStdString());
            auto artifacts = readNativeArtifacts(output, result.at("artifacts"), 64ull * 1024 * 1024);
            auto mesh = std::make_shared<NativeReplayMesh>(nativeReplayMesh(job, result, artifacts));
            QCOMPARE(result.at("data").at("shader_stage"), Json(stage.startsWith("ds") ? "ds" : stage.toStdString()));
            if (stage == "ds-uav-independent")
                QVERIFY(result.at("data").at("instance_selection").at("uav_independence").at("input_aliases_excluded") == true);
            QVERIFY(!mesh->positions.positions.empty());
            QVERIFY(!mesh->positions.faces.empty());
            QCOMPARE(mesh->positions.positions.size(), result.at("data").at("vertices").get<size_t>());
            if (stage == "vs" || stage == "gs") {
                QCOMPARE(mesh->positions.positions.size(), size_t(3));
                QCOMPARE(mesh->positions.faces.size(), size_t(1));
            }
            if (stage == "gs") {
                QCOMPARE(mesh->positions.positions[0], (std::array<double, 4>{-1, -1, 0, 1}));
                QCOMPARE(mesh->positions.positions[1], (std::array<double, 4>{-1, 3, 0, 1}));
                QCOMPARE(mesh->positions.positions[2], (std::array<double, 4>{3, -1, 0, 1}));
            }
            for (const auto &module : result.at("execution").at("loaded_modules")) {
                const auto path = QString::fromStdString(module.get<std::string>()).toLower();
                QVERIFY(!path.contains("renderdoc"));
                QVERIFY(!path.contains("intelswtools/gpa") && !path.contains("intelswtools\\gpa"));
            }
            QVERIFY(!view.findChild<QAction *>("replayMeshBackend")->isEnabled());
            const auto id = view.requestId();
            view.setBackendPath("C:/missing/renderdoc.dll");
            QCOMPARE(view.requestId(), id);
            QVERIFY(view.finishNative(id, result, mesh));
            auto table = view.findChild<QTableView *>("replayMeshPositions");
            QCOMPARE(table->model()->rowCount(), int(mesh->positions.positions.size()));
            QCOMPARE(table->model()->columnCount(), 5);
            const auto accepted = view.result();
            auto stale = result;
            stale["context_id"] = "stale";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finishNative(id, stale, mesh));
            QCOMPARE(view.result(), accepted);
            for (const auto &name :
                 {QString("vertices.bin"), QString("geometry.json"), QString("vertices.csv")}) {
                auto altered = artifacts;
                auto bytes = altered.value(name);
                QVERIFY(!bytes.isEmpty());
                bytes[0] = char(bytes[0] ^ 1);
                altered[name] = bytes;
                QVERIFY_THROWS_EXCEPTION(std::exception, nativeReplayMesh(job, result, altered));
            }
            auto staleJob = job;
            staleJob["request_id"] = "another-request";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nativeReplayMesh(staleJob, result, artifacts));
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     nativeReplayMesh(job, result, artifacts, [] { return true; }));
            write(output + "/vertices.bin", "changed after retention");
            for (const auto extension : {"csv", "obj", "json"}) {
                const auto path = directory.filePath(QString("export.") + extension);
                view.exportResult(path);
                const auto bytes = read(path);
                if (QString(extension) == "csv")
                    QCOMPARE(bytes, QByteArray::fromStdString(mesh->positions.csv()));
                else if (QString(extension) == "obj")
                    QCOMPARE(bytes, mesh->files.value("post_geometry.obj"));
                else {
                    auto exported = Json::parse(bytes.toStdString());
                    QCOMPARE(exported.at("presentation"), mesh->tables);
                    exported.erase("presentation");
                    QCOMPARE(exported, result);
                }
            }
            const auto retained = qEnvironmentVariable("FLORA_NATIVE_MESH_ARTIFACT_DIR");
            if (!retained.isEmpty()) {
                const auto folder = retained + '/' + QString::fromLatin1(QTest::currentDataTag());
                QVERIFY(QDir().mkpath(folder));
                for (auto it = mesh->files.cbegin(); it != mesh->files.cend(); ++it)
                    write(folder + '/' + it.key(), it.value());
                write(folder + "/job.json", QByteArray::fromStdString(job.dump(2)));
                write(folder + "/result.json", QByteArray::fromStdString(result.dump(2)));
            }
            view.setContext("changed-experiment", event);
            QVERIFY(!view.finishNative(id, result, mesh));
            QVERIFY(view.result().is_null());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void nativeMainWindow() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("ui.gpa_frame");
        testing::depthStencilCapture().save(capture);
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        adapter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto api = window.findChild<QTableView *>("apiLog");
        bool found = false;
        for (int row = 0; row < api->model()->rowCount(); ++row)
            if (api->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(api->model()->index(row, 0));
                found = true;
                break;
            }
        QVERIFY(found);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto view = window.findChild<ReplayMeshView *>();
        QVERIFY(view);
        view->setNativeMode(true);
        view->findChild<QAction *>("replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY2(tasks.takeLast()[0].toBool(),
                 qPrintable(view->findChild<QLabel *>("replayMeshSummary")->text()));
        QCOMPARE(view->result().at("context").at("action"), Json("mesh"));
        QCOMPARE(view->result().at("data").at("vertices"), Json(3));
        tasks.clear();
        view->findChild<QAction *>("replayMeshRead")->trigger();
        view->findChild<QAction *>("replayMeshCancel")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        QVERIFY(view->result().is_null());
        tasks.clear();
        view->findChild<QAction *>("replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.takeLast()[0].toBool());
    }
};
QTEST_MAIN(NativeReplayMeshUiTests)
#include "NativeReplayMeshUiTests.moc"
