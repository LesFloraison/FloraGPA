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
struct PresentationFixture {
    Json job, result;
    NativeArtifactBytes files;
};
PresentationFixture presentationFixture(PostTransformGeometry geometry) {
    geometry.report["sha256"] = sha256(geometry.bytes);
    const auto tables = postTransformTables(geometry);
    for (const auto key :
         {"vertex_references", "obj_vertices", "obj_faces", "obj_lines", "obj_points",
          "obj_unavailable_reason", "unique_vertices", "unique_identities", "conflicting_identities"})
        if (tables.contains(key))
            geometry.report[key] = tables.at(key);
    Json job{{"schema", nativeAnalysisSchema},
             {"backend", nativeAnalysisBackend},
             {"backend_sha256", "owned-fixture"},
             {"frame_sha256", "owned-fixture"},
             {"experiment_sha256", nullptr},
             {"action", "mesh"},
             {"options", {{"gpa_event", 1}, {"stage", geometry.report.at("requested_stage")}}},
             {"request_id", "owned-fixture"}};
    if (geometry.report.contains("instance_selection"))
        job["options"]["instance"] = geometry.report.at("instance_selection").at("instance");
    auto context = job;
    context.erase("request_id");
    Json result{{"schema", nativeAnalysisSchema},
                {"backend", nativeAnalysisBackend},
                {"ok", true},
                {"request_id", job.at("request_id")},
                {"context", context},
                {"context_id", analysisIdentity(context)},
                {"data", geometry.report}};
    auto files = postTransformFiles(geometry);
    Json identities = Json::array();
    for (const auto &identity : geometry.identities)
        identities.push_back({identity.instance, identity.vertexIndex, identity.vertexId});
    files["identities.json"] = QByteArray::fromStdString(identities.dump());
    return {job, result, files};
}
NativeReplayMesh present(PostTransformGeometry geometry) {
    const auto fixture = presentationFixture(std::move(geometry));
    return nativeReplayMesh(fixture.job, fixture.result, fixture.files);
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
            for (const auto stage : {"vs", "gs", "ds", "ds-uav", "ds-uav-independent", "gs-read"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(stage)))
                    << vendor << QString(stage);
    }
    void indexedPresentation_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode :
             {"repeated", "attribute-variant", "signed-zero", "nan-payload", "instances", "cut"})
            QTest::newRow(mode) << QString(mode);
    }
    void indexedPresentation() {
        try {
            QFETCH(QString, mode);
            const bool cut = mode == "cut", instances = mode == "instances";
            const bool variant =
                mode == "attribute-variant" || mode == "signed-zero" || mode == "nan-payload";
            const uint32_t count = cut ? 3 : 6;
            const Json inputs = cut         ? Json::array({5, nullptr, 2, 5})
                                : instances ? Json::array({5, 2, 5})
                                            : Json::array({5, 2, 5, 9, 2, 5});
            PostTransformGeometry geometry;
            geometry.report = {{"requested_stage", "vs-inputs"},
                               {"shader_stage", "vs"},
                               {"event", {{"id", 1}}},
                               {"stream", 0},
                               {"vertices", count},
                               {"primitives", count},
                               {"vertices_per_primitive", 1},
                               {"stride", 20},
                               {"input_topology", cut ? 5 : 4},
                               {"vertex_identity", {{"source", "owned-fixture"}}},
                               {"input_assembly",
                                {{"references", inputs},
                                 {"row_references", Json::array()},
                                 {"instance_count", instances ? 2 : 1}}},
                               {"attributes",
                                {{{"system_value", 1},
                                  {"component_type", 3},
                                  {"components", {0, 1, 2, 3}},
                                  {"component_count", 4},
                                  {"offset", 0},
                                  {"semantic", "SV_Position"},
                                  {"index", 0}},
                                 {{"system_value", 0},
                                  {"component_type", 3},
                                  {"components", {0}},
                                  {"component_count", 1},
                                  {"offset", 16},
                                  {"semantic", "VALUE"},
                                  {"index", 0}}}}};
            geometry.bytes.resize(count * 20);
            const std::array<float, 4> position{2, 4, 6, 2};
            for (uint32_t row = 0; row < count; ++row) {
                const uint32_t slot = cut ? (row ? row + 1 : 0) : instances ? row % 3 : row;
                const auto id = inputs[slot].get<uint32_t>();
                geometry.identities.push_back({instances ? row / 3 : 0, int64_t(id), id});
                geometry.report["input_assembly"]["row_references"].push_back(slot);
                std::memcpy(geometry.bytes.data() + row * 20, position.data(), 16);
                uint32_t value = mode == "nan-payload" ? 0x7fc00001u : 0;
                if (variant && row == 2)
                    value = mode == "signed-zero"   ? 0x80000000u
                            : mode == "nan-payload" ? 0x7fc00002u
                                                    : 0x3f800000u;
                std::memcpy(geometry.bytes.data() + row * 20 + 16, &value, 4);
            }
            const auto model = present(geometry);
            const auto sources = cut         ? Json::array({1, 0})
                                 : instances ? Json::array({1, 0, 4, 3})
                                 : variant   ? Json::array({1, 0, 2, 3})
                                             : Json::array({1, 0, 3});
            const auto references = cut         ? Json::array({1, 0, 1})
                                    : instances ? Json::array({1, 0, 1, 3, 2, 3})
                                    : variant   ? Json::array({1, 0, 2, 3, 0, 1})
                                                : Json::array({1, 0, 1, 2, 0, 1});
            QCOMPARE(model.tables.at("replay_mesh").at("vertex_sources"), sources);
            QCOMPARE(model.tables.at("replay_mesh").at("reference_vertices"), references);
            QCOMPARE(model.positions.positions.size(), sources.size());
            QCOMPARE(model.positions.indexCount, uint64_t(cut ? 4 : 6));
            QCOMPARE(model.positions.faces.size(), size_t(cut ? 0 : 2));
            for (size_t i = 0; i < model.positions.faces.size(); ++i)
                for (size_t j = 0; j < 3; ++j)
                    QCOMPARE(model.positions.faces[i][j], references[i * 3 + j].get<uint64_t>());
            QByteArray expected;
            for (const auto &source : sources)
                expected.append(
                    reinterpret_cast<const char *>(geometry.bytes.data() + source.get<size_t>() * 20), 20);
            QCOMPARE(model.files.value("post_vertices.bin"), expected);
            QCOMPARE(model.files.value("vertices.bin"),
                     QByteArray(reinterpret_cast<const char *>(geometry.bytes.data()),
                                qsizetype(geometry.bytes.size())));
            QCOMPARE(model.tables.at("tables").at("expanded_vertices").at("rows").size(), size_t(count));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
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
    void positionChannels_data() {
        QTest::addColumn<QByteArray>("channels");
        QTest::addColumn<bool>("nonfinite");
        QList<QByteArray> layouts;
        for (unsigned mask = 1; mask < 16; ++mask) {
            QByteArray layout;
            for (unsigned axis = 0; axis < 4; ++axis)
                if (mask & (1u << axis))
                    layout.append("xyzw"[axis]);
            layouts.push_back(layout);
        }
        layouts.append("zxy");
        layouts.append("wzxy");
        for (const auto &layout : layouts)
            for (bool special : {false, true})
                QTest::newRow(
                    qPrintable(QString("%1-special-%2").arg(QString::fromLatin1(layout)).arg(special)))
                    << layout << special;
    }
    void positionChannels() {
        QFETCH(QByteArray, channels);
        QFETCH(bool, nonfinite);
        Json indices = Json::array();
        for (char channel : channels)
            indices.push_back(QByteArray("xyzw").indexOf(channel));
        PostTransformGeometry geometry;
        geometry.report = {{"requested_stage", "vs-inputs"},
                           {"shader_stage", "vs"},
                           {"event", {{"id", 1}}},
                           {"stream", 0},
                           {"vertices", 3},
                           {"primitives", 1},
                           {"vertices_per_primitive", 3},
                           {"stride", 24},
                           {"instance_selection", {{"instance", 0}}},
                           {"attributes",
                            {{{"system_value", 1},
                              {"component_type", 3},
                              {"components", indices},
                              {"component_count", channels.size()},
                              {"offset", 4},
                              {"semantic", "SV_Position"},
                              {"index", 0}}}}};
        const std::array<std::array<float, 4>, 3> values{
            {{2, 4, 6, 2},
             {-0.f, 1, 2, 0},
             {nonfinite ? std::numeric_limits<float>::infinity() : 3.f, 2, 1, 1}}};
        geometry.bytes.resize(72, 0xa5);
        for (size_t row = 0; row < 3; ++row)
            for (size_t c = 0; c < size_t(channels.size()); ++c) {
                const auto value = values[row][indices[c].get<size_t>()];
                std::memcpy(geometry.bytes.data() + row * 24 + 4 + 4 * c, &value, 4);
            }
        auto fixture = presentationFixture(geometry);
        fixture.files["post_geometry.obj"] = "Untrusted stale presentation";
        auto mesh =
            std::make_shared<NativeReplayMesh>(nativeReplayMesh(fixture.job, fixture.result, fixture.files));
        QCOMPARE(mesh->tables.at("replay_mesh").at("position_components"), indices);
        QCOMPARE(mesh->files.value("post_vertices.bin"),
                 QByteArray(reinterpret_cast<const char *>(geometry.bytes.data()), geometry.bytes.size()));
        const bool xyz = channels.contains('x') && channels.contains('y') && channels.contains('z');
        const bool prefix = QByteArray("xyzw").startsWith(channels);
        const bool obj = xyz || prefix;
        QCOMPARE(mesh->files.contains("post_geometry.obj"), obj);
        QCOMPARE(mesh->preview.empty(), !xyz || nonfinite);
        if (xyz && !nonfinite) {
            QCOMPARE(mesh->preview.at("positions")[0],
                     channels.contains('w') ? Json::array({1., 2., 3.}) : Json::array({2., 4., 6.}));
            QCOMPARE(mesh->preview.at("positions")[1], Json::array({-0., 1., 2.}));
        }
        ReplayMeshView view;
        view.setNativeMode(true);
        view.setContext("owned-channel-fixture", 1);
        QVERIFY(view.finishNative(view.requestId(), fixture.result, mesh));
        auto table = view.findChild<QTableView *>("replayMeshPositions")->model();
        QCOMPARE(table->columnCount(), int(channels.size() + 1));
        for (int c = 0; c < channels.size(); ++c) {
            QCOMPARE(table->headerData(c + 1, Qt::Horizontal).toString(), QString(QChar(channels[c])));
            for (int row = 0; row < 3; ++row)
                QCOMPARE(table->data(table->index(row, c + 1)).toString(),
                         QString::fromStdString(replayMeshFloat(values[row][indices[c].get<size_t>()])));
        }
        QTemporaryDir directory;
        view.exportResult(directory.filePath("mesh.csv"));
        view.exportResult(directory.filePath("mesh.json"));
        QCOMPARE(read(directory.filePath("mesh.csv")), mesh->files.value("post_vertices.csv"));
        QCOMPARE(Json::parse(read(directory.filePath("mesh.json")).toStdString())
                     .at("presentation")
                     .at("replay_mesh")
                     .at("position_components"),
                 indices);
        if (obj) {
            view.exportResult(directory.filePath("mesh.obj"));
            QCOMPARE(read(directory.filePath("mesh.obj")), mesh->files.value("post_geometry.obj"));
        } else {
            write(directory.filePath("mesh.obj"), "Keep existing export");
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.exportResult(directory.filePath("mesh.obj")));
            QCOMPARE(read(directory.filePath("mesh.obj")), QByteArray("Keep existing export"));
            QVERIFY(!mesh->tables.at("replay_mesh").at("obj_unavailable_reason").is_null());
        }
        if (const auto root = qEnvironmentVariable("FLORA_NATIVE_MESH_CHANNEL_EVIDENCE"); !root.isEmpty()) {
            QDir output(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag())));
            QVERIFY(output.mkpath("."));
            write(output.filePath("job.json"), QByteArray::fromStdString(fixture.job.dump(2)));
            write(output.filePath("result.json"), QByteArray::fromStdString(fixture.result.dump(2)));
            write(output.filePath("preview.json"), QByteArray::fromStdString(mesh->preview.dump(2)));
            for (auto it = mesh->files.cbegin(); it != mesh->files.cend(); ++it)
                write(output.filePath(it.key()), it.value());
            write(output.filePath("presentation.json"), read(directory.filePath("mesh.json")));
        }
        auto empty = geometry;
        empty.bytes.clear();
        empty.report["vertices"] = 0;
        empty.report["primitives"] = 0;
        const auto emptyFixture = presentationFixture(empty);
        const auto emptyMesh = nativeReplayMesh(emptyFixture.job, emptyFixture.result, emptyFixture.files);
        QCOMPARE(emptyMesh.preview.empty(), !xyz);
        for (const auto &invalid :
             {Json::array({0, 0}), Json::array({0, 4}), Json::array({0, -1}), Json::array({0, 1.5})}) {
            auto malformed = geometry;
            malformed.report["attributes"][0]["components"] = invalid;
            malformed.report["attributes"][0]["component_count"] = invalid.size();
            QVERIFY_THROWS_EXCEPTION(std::exception, present(malformed));
        }
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
            if (stage == "gs-read")
                fixture = testing::geometryResourceReadCapture(false, false);
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
            QCOMPARE(result.at("data").at("shader_stage"), Json(stage.startsWith("ds")   ? "ds"
                                                                : stage.startsWith("gs") ? "gs"
                                                                                         : "vs"));
            if (stage == "ds-uav-independent")
                QVERIFY(result.at("data")
                            .at("instance_selection")
                            .at("uav_independence")
                            .at("input_aliases_excluded") == true);
            QVERIFY(!mesh->positions.positions.empty());
            QVERIFY(!mesh->positions.faces.empty());
            QCOMPARE(mesh->positions.positions.size(), result.at("data").at("vertices").get<size_t>());
            if (stage == "vs" || stage.startsWith("gs")) {
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
