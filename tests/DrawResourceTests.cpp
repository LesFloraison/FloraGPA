#include "DepthStencilCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/OutputStorageExport.h"
#include "app/PixelHistoryView.h"
#include "application/DrawResources.h"
#include "application/FrameOutput.h"
#include "application/RdcEvents.h"
#include "application/RdcJobs.h"
#include "application/ReplaySession.h"
#include <QAction>
#include <QImage>
#include <QLabel>
#include <QSettings>
#include <QProcess>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidgetItemIterator>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
class ResourceSession final : public IReplaySession {
  public:
    std::map<Id, SessionResource> objects;
    ReplayBackend backend() const override { return ReplayBackend::Dx12; }
    const std::string &identity() const override { static const std::string id = "fixture"; return id; }
    const std::filesystem::path &path() const override { static const std::filesystem::path p; return p; }
    Json information() const override { return {}; }
    std::span<const Id> events() const override { return {}; }
    SessionEvent event(Id) const override { return {}; }
    Json eventDetails(Id) const override { return {}; }
    std::string argumentText(Id) const override { return {}; }
    Json executionDetails(Id) const override { return {}; }
    Json submissionDetails() const override { return {}; }
    std::span<const Id> resources() const override { return {}; }
    SessionResource resource(Id id) const override { return objects.at(id); }
    Json resourceDetails(Id) const override { return {}; }
    SessionCapability capability(ReplayFeature, const ReplaySelection &) const override { return {}; }
};
Capture fixture(bool sparse = false) {
    auto c = depthStencilCapture();
    c.add(99, 5, 0x8c, statePack(Id(0), Id(0), Id(20), 28u, 4u, 0u, 1u, 0u, 0u));
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = sparse ? 4 : 1;
    s.rtv[sparse ? 3 : 0] = 21;
    s.dsv = 722;
    s.rasterizer = 724;
    s.viewports = 22;
    s.stages[0].shader = 30;
    s.stages[4].shader = 32;
    s.stages[4].srv[0] = 99;
    if (sparse) {
        s.stages[0].srv[4] = 99;
        s.stages[4].srv[5] = UINT64_MAX;
    }
    for (auto &e : c.entries)
        if (e.id == 990 || e.id == 1990) {
            const auto payload = snapshot(s);
            std::copy(payload.begin(), payload.end(), c.bytes.begin() + ptrdiff_t(e.offset));
        }
    return c;
}
} // namespace
class DrawResourceTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-DrawResourceTests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings().clear();
        applyAppearance(*qApp);
    }
    void inventory() {
        QTemporaryDir dir;
        fixture(true).save(dir.filePath("frame.gpa_frame"));
        Frame frame(dir.filePath("frame.gpa_frame").toStdWString());
        const auto rows = drawResources(frame, 1000);
        const auto report = drawResourceInventory(rows, 1000);
        QCOMPARE(report["counts"]["input_textures"], Json(1));
        QCOMPARE(report["counts"]["input_bindings"], Json(3));
        QCOMPARE(report["counts"]["rtv"], Json(1));
        QCOMPARE(report["counts"]["dsv"], Json(1));
        auto unknown =
            std::find_if(rows.begin(), rows.end(), [](const auto &b) { return b.image.view == UINT64_MAX; });
        QVERIFY(unknown != rows.end());
        QVERIFY(!unknown->error.empty());
        auto output = std::find_if(rows.begin(), rows.end(), [](const auto &b) { return b.kind == "RTV"; });
        QVERIFY(output != rows.end());
        QCOMPARE(output->slot, 3u);
        QCOMPARE(output->image.boundary, ImageBoundary::After);
        ReplayOptions edit;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {7, 1};
        edit.srvEdits[1000][{4, 0}] = desc;
        const auto changed = drawResources(frame, 1000, edit);
        auto invalid = std::find_if(changed.begin(), changed.end(),
                                    [](const auto &b) { return b.key == "in/PS/SRV/0"; });
        QVERIFY(invalid != changed.end());
        QVERIFY(!invalid->error.empty());
        QVERIFY_EXCEPTION_THROWN(
            exportDrawResources(frame, 1000, {}, {{"previews", {"unknown"}}}, dir.path().toStdWString()),
            std::exception);
    }
    void dx12SubresourceRanges() {
        Json d{{"Dimension", 3}, {"Width", "512"}, {"Height", 256}, {"MipLevels", 3},
               {"DepthOrArraySize", 2}, {"SampleCount", 1}, {"Format", 45}};
        auto r = dx12TextureRange(d);
        QCOMPARE(r.count(), 12u);
        QCOMPARE(r.index(2, 1, 1), 11u);
        QVERIFY_EXCEPTION_THROWN(r.index(3, 0, 0), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(r.index(0, 2, 0), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(r.index(0, 0, 2), std::runtime_error);
        d["Dimension"] = 4; d["DepthOrArraySize"] = 16; d["Format"] = 28;
        r = dx12TextureRange(d);
        QCOMPARE(r.layers, 1u); QCOMPARE(r.depth, 16u); QCOMPARE(r.count(), 3u);
        QVERIFY_EXCEPTION_THROWN(r.index(0, 1, 0), std::runtime_error);
        d["Dimension"] = 3; d["MipLevels"] = 1; d["DepthOrArraySize"] = 1;
        r = dx12TextureRange(d); QCOMPARE(r.count(), 1u);
        QVERIFY_EXCEPTION_THROWN(r.index(4, 0, 0), std::runtime_error);
        d["MipLevels"] = 0; QCOMPARE(dx12TextureRange(d).mips, 10u);
        d["Width"] = "512bad"; QVERIFY_EXCEPTION_THROWN(dx12TextureRange(d), std::runtime_error);
    }
    void dx12ResourceBoundaryIdentity() {
        const Json b{{"schema", 1}, {"execution", "18446744073709551615"}, {"boundary", "before"},
                     {"isolate_missing_queries", false}};
        const Json node{{"customName", rdcResourceBoundaryMarker(b)}, {"eventId", uint32_t(19)},
                        {"children", Json::array()}};
        const Json job{{"execution", b.at("execution")}, {"boundary", "before"}, {"isolate_missing_queries", false}};
        const auto result = readRdcResourceBoundary(Json::array({node}), job);
        QCOMPARE(result.at("event_id"), Json(19));
        auto wrong = job; wrong["boundary"] = "after";
        QVERIFY_EXCEPTION_THROWN(readRdcResourceBoundary(Json::array({node}), wrong), std::runtime_error);
        wrong = job; wrong["execution"] = "9007199254740993";
        QVERIFY_EXCEPTION_THROWN(readRdcResourceBoundary(Json::array({node}), wrong), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readRdcResourceBoundary(Json::array({node, node}), job), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(readRdcResourceBoundary(Json::array(), job), std::runtime_error);
    }
    void dx12CorruptResourceRequests() {
        Json d{{"Dimension", 3}, {"Width", "512"}, {"Height", 256}, {"MipLevels", 3},
               {"DepthOrArraySize", 2}, {"SampleCount", 1}, {"Format", 45}};
        Json request{{"resource_description", d}, {"creation", "18446744073709551615"},
                     {"object", "9007199254740993"}, {"buffer", false}, {"thumbnail", false},
                     {"mip", 1}, {"layer", 1}, {"slice", 0}, {"sample", UINT32_MAX},
                     {"plane", "stencil"}, {"channel", "RGBA"}, {"low", 0.}, {"high", 255.}};
        validateDx12ResourceRequest(request);
        for (const auto &invalid : {Json(-1), Json(0.5), Json(uint64_t(UINT32_MAX) + 1), Json("1"), Json(4)}) {
            auto bad = request; bad["mip"] = invalid;
            QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception);
        }
        auto bad = request; bad["object"] = 9007199254740993ULL;
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception);
        bad = request; bad["resource_description"]["Format"] = 40;
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception);
        bad = request; bad["sample"] = 1;
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception);
        bad = request; bad["resource_description"]["SampleCount"] = 4;
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception); // MSAA cannot have mip chains.
        bad["resource_description"]["MipLevels"] = 1; bad["mip"] = 0; bad["sample"] = 3;
        validateDx12ResourceRequest(bad);
        bad["sample"] = 4;
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception);
        bad = request; bad["view_range"] = {{"mip_first", 2}, {"mip_end", 3},
            {"layer_first", 0}, {"layer_end", 2}, {"slice_first", 0}, {"slice_end", 1}};
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(bad), std::exception);
        request["resource_description"]["Dimension"] = 4; request["resource_description"]["DepthOrArraySize"] = 8;
        request["resource_description"]["Format"] = 28; request["plane"] = "color";
        request["layer"] = 0; request["slice"] = 3;
        validateDx12ResourceRequest(request); // Slice 3 is in mip 1, not array layer 3.
        request["slice"] = 4;
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(request), std::exception);
        request = {{"resource_description", {{"Dimension", 1}, {"Width", "1024"}}},
                   {"creation", "1"}, {"object", "2"}, {"buffer", true}, {"thumbnail", false},
                   {"offset", "1023"}, {"length", "1"}};
        validateDx12ResourceRequest(request);
        request["length"] = "2";
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(request), std::exception);
        request["offset"] = "18446744073709551615";
        QVERIFY_EXCEPTION_THROWN(validateDx12ResourceRequest(request), std::exception);
    }
    void dx12BindingIdentityAndViews() {
        ResourceSession session;
        constexpr Id creation = UINT64_MAX, object = 9007199254740993ULL, execution = UINT64_MAX - 2;
        Json desc{{"Dimension", 3}, {"Width", "128"}, {"Height", 64}, {"MipLevels", 3},
                  {"DepthOrArraySize", 4}, {"SampleCount", 1}, {"Format", 28}};
        session.objects[creation] = {creation, object, "Texture", desc.dump()};
        session.objects[4] = {4, 44, "Buffer", Json{{"Dimension", 1}, {"Width", "1024"}}.dump()};
        Json ref{{"mapping_status", "capture_resource_marker"}, {"resource_creation", std::to_string(creation)},
                 {"resource_object", std::to_string(object)}};
        Json view{{"first_mip", 1}, {"mip_count", 2}, {"first_slice", 2}, {"slice_count", 1}};
        Json rows = Json::array({
            {{"section", "OM"}, {"binding", "RTV 0"}, {"resource", ref}, {"details", view}},
            {{"section", "OM"}, {"binding", "RTV 3"}, {"resource", ref}, {"details", view}},
            {{"section", "PS"}, {"binding", "texture[3]"}, {"resource", ref},
             {"details", {{"array_element", 3}, {"reflection_binding", {{"register", 7}, {"space", 2}}},
                          {"descriptor", view}}}}});
        rows[2]["details"]["descriptor"]["category"] = "SRV";
        auto alias = rows[2]; alias["details"]["array_element"] = 4; rows.push_back(alias);
        auto invalid = rows[0]; invalid["binding"] = "RTV 1";
        invalid["resource"]["resource_object"] = "9007199254740994"; rows.push_back(invalid);
        invalid = rows[0]; invalid["binding"] = "RTV 2"; invalid["resource"]["mapping_status"] = "unmapped";
        rows.push_back(invalid);
        for (const auto kind : {"CBV", "SRV", "UAV", "VB", "IB"}) {
            auto row = rows[2]; row["resource"] = {{"mapping_status", "capture_resource_marker"},
                {"resource_creation", "4"}, {"resource_object", "44"}};
            row["details"]["descriptor"] = {{"category", kind}, {"byte_offset", "128"}, {"byte_size", "256"}};
            if (std::string(kind) == "VB" || std::string(kind) == "IB") {
                row["section"] = "IA"; row["binding"] = std::string(kind) == "VB" ? "VB 2" : "Index buffer";
                row["details"] = row.at("details").at("descriptor");
            }
            rows.push_back(row);
        }
        const auto bindings = dx12DrawResources(session, execution, {{"pipeline_rows", rows}});
        QCOMPARE(bindings.size(), size_t(11));
        QCOMPARE(bindings[0].image.resource, creation);
        QCOMPARE(bindings[0].image.mip, 1u); QCOMPARE(bindings[0].image.layer, 2u);
        QCOMPARE(bindings[0].mipEnd, 3u); QCOMPARE(bindings[0].layerEnd, 3u);
        QCOMPARE(bindings[1].slot, 3u);
        QVERIFY(bindings[2].key != bindings[3].key);
        QCOMPARE(bindings[2].slot, 7u);
        QCOMPARE(bindings[2].image.boundary, ImageBoundary::Before);
        QVERIFY(!bindings[4].error.empty()); QVERIFY(!bindings[5].error.empty());
        QCOMPARE(bindings[8].image.boundary, ImageBoundary::After);
        for (size_t i = 6; i < bindings.size(); ++i) { QVERIFY(!bindings[i].texture); QVERIFY(bindings[i].error.empty()); }
        const auto exported = drawResourceJson(bindings.front());
        QCOMPARE(exported.at("resource"), Json(std::to_string(creation)));
        QCOMPARE(exported.at("event"), Json(std::to_string(execution)));
        const auto repeated = dx12DrawResources(session, execution - 1, {{"pipeline_rows", rows}});
        QCOMPARE(repeated.front().image.event, execution - 1);
        QVERIFY(drawResourceJson(repeated.front()) != exported);
        desc["Format"] = 19; session.objects[creation].summary = desc.dump();
        const auto depthViews = dx12DrawResources(session, execution, {{"pipeline_rows", Json::array({rows[2]})}});
        QVERIFY(depthViews.front().error.find("SRV plane") != std::string::npos);
    }
    void unifiedResourceEntry() {
        MainWindow window;
        auto center = window.findChild<QTabWidget *>("analysisTabs");
        QVERIFY(center);
        int resources = 0;
        for (auto tabs : window.findChildren<QTabWidget *>())
            for (int i = 0; i < tabs->count(); ++i) resources += tabs->tabText(i) == "Resources";
        QCOMPARE(resources, 1);
        auto scope = window.findChild<QComboBox *>("resourceScope");
        QVERIFY(scope); QCOMPARE(scope->count(), 2);
        auto global = window.findChild<QTableView *>("resources");
        QVERIFY(global);
        auto workspace = window.findChild<QWidget *>("resourceWorkspace");
        QVERIFY(workspace->isAncestorOf(global));
        QVERIFY(workspace->isAncestorOf(window.findChild<QLineEdit *>("bufferOffset")));
    }
    void dx12KnownBoundaryValues() {
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        if (library.isEmpty()) QSKIP("Set a compatible RenderDoc library for the independent DX12 oracle");
        QTemporaryDir temp;
        const auto root = qEnvironmentVariable("FLORA_RESOURCE_FIXTURE_OUTPUT", temp.path());
        QVERIFY(QDir().mkpath(root));
        const auto run = [&](const QString &exe, const QStringList &args) {
            QProcess process; process.start(QCoreApplication::applicationDirPath() + "/" + exe, args);
            if (!process.waitForStarted() || !process.waitForFinished(120000) || process.exitCode() ||
                process.exitStatus() != QProcess::NormalExit)
                throw std::runtime_error(process.readAllStandardError().toStdString());
        };
        const auto read = [](const QString &file) {
            QFile input(file); if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing oracle artifact");
            return input.readAll();
        };
        QVERIFY(QDir().mkpath(root + "/full"));
        run("FloraResourceWorkspaceFixture.exe", {library, root + "/full/oracle", "full"});
        const auto full = root + "/full/" + QDir(root + "/full").entryList({"*.rdc"}, QDir::Files).front();
        for (const auto boundary : {"before", "after"}) {
            const auto dir = root + "/" + boundary; QVERIFY(QDir().mkpath(dir));
            run("FloraResourceWorkspaceFixture.exe", {library, dir + "/oracle", boundary});
            const auto captures = QDir(dir).entryList({"*.rdc"}, QDir::Files);
            QCOMPARE(captures.size(), 1);
            const auto capture = dir + "/" + captures.front();
            Json description{{"Dimension", 3}, {"Width", "16"}, {"Height", 16}, {"MipLevels", 2},
                             {"DepthOrArraySize", 2}, {"SampleCount", 1}, {"Format", 28}};
            Json color{{"creation", "18446744073709551613"}, {"object", "9007199254740993"},
                       {"resource_description", description}, {"buffer", false}, {"thumbnail", false},
                       {"key", "color"}, {"mip", 1}, {"layer", 1}, {"slice", 0}, {"sample", UINT32_MAX},
                       {"plane", "color"}, {"channel", "RGBA"}, {"low", 0.}, {"high", 1.}};
            auto depth = color; depth["creation"] = "18446744073709551614"; depth["object"] = "9007199254740994";
            depth["key"] = "depth"; depth["plane"] = "depth"; depth["mip"] = 0; depth["layer"] = 0;
            depth["resource_description"] = {{"Dimension", 3}, {"Width", "8"}, {"Height", 8}, {"MipLevels", 1},
                                            {"DepthOrArraySize", 1}, {"SampleCount", 1}, {"Format", 45}};
            auto stencil = depth; stencil["key"] = "stencil"; stencil["plane"] = "stencil";
            auto thumbnail = color; thumbnail["thumbnail"] = true; thumbnail["key"] = "thumbnail";
            auto duplicate = thumbnail; duplicate["key"] = "another-slot";
            auto fast = thumbnail; fast["key"] = "fast"; fast["output"] = "thumbnail";
            auto preview = color; preview["key"] = "display"; preview["output"] = "preview";
            auto storage = color; storage["key"] = "storage"; storage["output"] = "storage";
            Json buffer{{"creation", "18446744073709551615"}, {"object", "9007199254740995"},
                        {"resource_description", {{"Dimension", 1}, {"Width", "256"}}},
                        {"key", "buffer"}, {"buffer", true}, {"thumbnail", false}, {"offset", "4"}, {"length", "8"}};
            auto job = prepareRdcJob(capture, "resource-workspace", dir + "/read", library,
                {{"execution", "9007199254740999"}, {"boundary", boundary},
                 {"resource_requests", Json::array({color, depth, stencil, buffer, thumbnail, duplicate, fast, preview, storage})}});
            const auto jobPath = dir + "/job.json";
            { QFile output(jobPath); QVERIFY(output.open(QIODevice::WriteOnly)); output.write(QByteArray::fromStdString(job.dump(2))); }
            run("FloraGPA.Rdc.exe", {"--job", jobPath});
            const auto result = Json::parse(read(dir + "/read/result.json").toStdString());
            QVERIFY2(result.at("ok").get<bool>(), result.dump().c_str());
            QCOMPARE(result.at("execution"), Json("9007199254740999"));
            const auto &rows = result.at("resource_results");
            const bool after = std::string(boundary) == "after";
            for (const auto &row : rows) QVERIFY2(!row.contains("error"), row.dump().c_str());
            const auto image = [&](size_t i) { return QImage(dir + "/read/" + QString::fromStdString(rows[i].at("image"))); };
            QCOMPARE(image(0).size(), QSize(8, 8));
            QVERIFY(std::abs(image(0).pixelColor(0, 0).red() - (after ? 191 : 64)) <= 1);
            QVERIFY(std::abs(image(1).pixelColor(0, 0).red() - (after ? 191 : 64)) <= 1);
            QVERIFY2(std::abs(image(2).pixelColor(0, 0).red() - (after ? 201 : 37)) <= 1,
                     qPrintable(QString("Stencil display = %1").arg(image(2).pixelColor(0, 0).red())));
            const auto raw = read(dir + "/read/" + QString::fromStdString(rows[3].at("file")));
            QCOMPARE(raw.size(), 8); uint32_t value{}; memcpy(&value, raw.constData(), 4);
            QCOMPARE(value, after ? 42u : 7u);
            QCOMPARE(rows[4].at("image"), rows[5].at("image")); // Duplicate bindings share the batch read.
            QVERIFY(image(4).width() <= 96 && image(4).height() <= 96);
            QVERIFY(rows[0].contains("storage_description"));
            QCOMPARE(rows[6].at("display_path"), Json("bounded_gpu_thumbnail"));
            QVERIFY(image(6).width() <= 96 && image(6).height() <= 96);
            QVERIFY(!rows[6].contains("file"));
            QCOMPARE(image(7), image(0));
            QVERIFY(!rows[7].contains("file"));
            QCOMPARE(rows[7].at("storage_read"), Json(false));
            QVERIFY(!QFile::exists(dir + "/read/resource-7.bin"));
            QVERIFY(!rows[8].contains("image"));
            QVERIFY(!QFile::exists(dir + "/read/resource-8.png"));
            QCOMPARE(rows[8].at("sha256"), rows[0].at("sha256"));
            auto exportJob = job;
            exportJob["resource_requests"] = Json::array({storage});
            exportJob["experiment_key"] = "immutable-export-context";
            exportJob["capture_identity"] = "oracle-lifetime";
            Json exportDisplay{{"resource", storage.at("creation")}, {"execution", job.at("execution")},
                               {"boundary", boundary}, {"experiment_key", "immutable-export-context"}};
            exportDeferredRdcStorage(exportJob, QCoreApplication::applicationDirPath() + "/FloraGPA.Rdc.exe",
                                     dir + "/export.bin", exportDisplay);
            QCOMPARE(read(dir + "/export.bin"), read(dir + "/read/" + QString::fromStdString(rows[8].at("file"))));
            const auto exported = Json::parse(read(dir + "/export.bin.json").toStdString());
            QCOMPARE(exported.at("storage_sha256"), rows[8].at("sha256"));
            QCOMPARE(exported.at("boundary"), Json(boundary));
            QCOMPARE(exported.at("resource"), storage.at("creation"));
            QCOMPARE(exported.at("experiment_key"), Json("immutable-export-context"));
            auto reuse = job; reuse["capture"] = full.toStdString(); reuse["analysis_frame"] = true;
            reuse["out"] = (dir + "/reuse").toStdString();
            const auto reuseReport = Json::parse(read(runRdcAnalysis(reuse,
                QCoreApplication::applicationDirPath() + "/FloraGPA.Rdc.exe", 120)).toStdString());
            const auto &reused = reuseReport.at("resource_results");
            for (size_t i = 0; i < rows.size(); ++i) {
                QVERIFY2(!reused[i].contains("error"), reused[i].dump().c_str());
                if (rows[i].contains("sha256")) QCOMPARE(reused[i].at("sha256"), rows[i].at("sha256"));
                if (rows[i].contains("image")) QCOMPARE(QImage(dir + "/reuse/" + QString::fromStdString(reused[i].at("image"))), image(i));
            }
            qInfo() << boundary << "color/depth/stencil/buffer and mip 1 array 1 verified";
        }
    }
    void rangesBuffersAndNoRt() {
        QTemporaryDir dir;
        auto capture = stateCapture();
        State state{};
        state.omStart = 8;
        state.stages[4].srv[0] = 6;
        state.stages[5].srv[7] = 23;
        state.csUav[0] = 22;
        state.csUav[63 % 8] = 22;
        state.csExtended[55] = 22;
        capture.add(9000, 3, 3, snapshot(state));
        capture.add(9001, 7, 0x37, statePack(Id(9000), Id(0), Id(1), 3u, 0u));
        capture.save(dir.filePath("frame.gpa_frame"));
        Frame frame(dir.filePath("frame.gpa_frame").toStdWString());
        const auto rows = drawResources(frame, 9001);
        const auto report = drawResourceInventory(rows, 9001);
        QCOMPARE(report["counts"]["rtv"], Json(0));
        QCOMPARE(report["counts"]["dsv"], Json(0));
        QCOMPARE(report["counts"]["uav"], Json(3));
        QCOMPARE(report["counts"]["input_textures"], Json(1));
        QCOMPARE(report["counts"]["input_bindings"], Json(2));
        const auto &texture = rows.front();
        QVERIFY(texture.error.empty());
        QCOMPARE(texture.mipEnd, 1u);
        QCOMPARE(texture.layerEnd, 1u);
        auto buffer =
            std::find_if(rows.begin(), rows.end(), [](const auto &b) { return b.image.view == 23; });
        QVERIFY(buffer != rows.end());
        QVERIFY(buffer->error.empty());
        QVERIFY(!buffer->texture);
        QVERIFY_EXCEPTION_THROWN(
            exportDrawResources(frame, 9001, {}, {{"previews", Json::array({1})}}, dir.path().toStdWString()),
            std::exception);
    }
    void staleThumbnails() {
        ResourceBrowser browser;
        browser.resize(300, 700);
        browser.show();
        DrawResourceBinding b;
        b.key = "in/PS/SRV/0";
        b.kind = "SRV";
        b.stage = "PS";
        b.texture = true;
        b.width = b.height = 32;
        b.image.event = 1;
        b.image.resource = UINT64_MAX;
        browser.setContext("first", {b}, 1);
        browser.select(b.key);
        QVERIFY(!browser.nextPreviews()["previews"].empty());
        browser.setContext("second", {b}, 1);
        browser.acceptPreviews("first", {}); // Stale prepared results are ignored.
        QCOMPARE(browser.contextKey(), QString("second"));
        QVERIFY(!browser.nextPreviews()["previews"].empty());
        browser.failPreviews("second", "Selection changed");
        QVERIFY(!browser.nextPreviews()["previews"].empty());
    }
    void invalidThumbnailBatchDoesNotPublish() {
        QTemporaryDir dir;
        ResourceBrowser browser;
        browser.resize(350, 700); browser.show();
        std::vector<DrawResourceBinding> bindings(2);
        for (unsigned i = 0; i < bindings.size(); ++i) {
            auto &b = bindings[i];
            b.key = "in/PS/SRV/" + std::to_string(i);
            b.kind = "SRV"; b.stage = "PS"; b.slot = i; b.texture = true;
            b.width = b.height = 32; b.image.event = 1; b.image.resource = 10 + i;
        }
        browser.setContext("atomic", bindings, 1);
        browser.select(bindings[0].key);
        QCOMPARE(browser.nextPreviews().at("previews").size(), size_t(2));
        auto tree = browser.findChild<QTreeWidget *>("drawResources");
        QTreeWidgetItem *first = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole).toString() == QString::fromStdString(bindings[0].key)) first = *it;
        QVERIFY(first);
        const auto originalIcon = first->icon(0).cacheKey();
        QImage image(32, 32, QImage::Format_RGBA8888); image.fill(Qt::red);
        QVERIFY(image.save(dir.filePath("first.png")));
        auto report = drawResourceInventory(bindings, 1);
        report["bindings"][0]["preview"] = "first.png";
        report["bindings"][1]["preview"] = "missing.png";
        QFile file(dir.filePath("report.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QByteArray::fromStdString(report.dump());
        QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QVERIFY_THROWS_EXCEPTION(std::exception, browser.acceptPreviews("atomic", readThumbnailOutput(dir.path())));
        QCOMPARE(first->icon(0).cacheKey(), originalIcon);
    }
    void boundaryPreviews() {
        QTemporaryDir dir;
        fixture().save(dir.filePath("frame.gpa_frame"));
        Frame frame(dir.filePath("frame.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 1000;
        const auto report = exportDrawResources(
            frame, 1000, options, {{"previews", {"in/PS/SRV/0", "out/OM/RTV/0"}}}, dir.path().toStdWString());
        QImage input, output;
        for (const auto &b : report["bindings"]) {
            if (b["key"] != "in/PS/SRV/0" && b["key"] != "out/OM/RTV/0")
                continue;
            QVERIFY2(b.contains("preview"), b.dump().c_str());
            QImage image(dir.filePath(QString::fromStdString(b["preview"])));
            QVERIFY(!image.isNull());
            QVERIFY(image.width() <= 96 && image.height() <= 96);
            (b["role"] == "input" ? input : output) = image;
        }
        QVERIFY(input != output);
        QCOMPARE(input.pixelColor(0, 0).red(), 0);
        Replay baseline(frame, options);
        baseline.run();
        const auto raw = readFrameOutput(baseline, 20, {}, 21).image;
        QCOMPARE(output.pixelColor(0, 0).red(), int(raw.rgba[0]));
    }
    void overlayAndCoordinates() {
        ImageView view;
        view.resize(480, 320);
        view.show();
        QImage base(32, 16, QImage::Format_RGBA8888);
        base.fill(QColor(12, 34, 56));
        view.setImage(base);
        QImage mask(base.size(), QImage::Format_RGBA8888);
        mask.fill(Qt::black);
        mask.setPixelColor(3, 4, Qt::white);
        view.setOverlayMask(mask);
        QVERIFY(view.hasOverlay());
        QCOMPARE(view.image(), base);
        QSignalSpy picked(&view, &ImageView::pixelSelected);
        view.setPixelPicking(true);
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                          view.mapFromScene(QPointF(3.5, 4.5)));
        QCOMPARE(picked.count(), 1);
        QCOMPARE(picked[0][0].toInt(), 3);
        QCOMPARE(picked[0][1].toInt(), 4);
        QCOMPARE(qvariant_cast<QColor>(picked[0][2]), QColor(12, 34, 56));
        view.clearOverlay();
        QVERIFY(!view.hasOverlay());
        view.setOverlayMask(QImage(1, 1, QImage::Format_RGBA8888));
        QVERIFY(!view.hasOverlay());
        picked.clear();
        const auto start = view.mapFromScene(QPointF(8.5, 5.5));
        QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(view.viewport(), start + QPoint(40, 20));
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 20));
        QVERIFY(picked.empty());
    }
    void historyQueryAndStaleResult() {
        PixelHistoryView view;
        view.setContext("fixture", 1000);
        QSignalSpy requests(&view, &PixelHistoryView::readRequested);
        view.queryPixel(1000, UINT64_MAX, 3, 4, 1, 2, 0);
        QCOMPARE(requests.count(), 1);
        const auto old = view.requestId();
        QCOMPARE(view.request().at("resource"), Json(UINT64_MAX));
        QCOMPARE(view.request().at("x"), Json(3));
        view.queryPixel(1000, 20, 8, 9, 0, 0, 1);
        QCOMPARE(requests.count(), 2);
        QVERIFY(!view.finish(old, {{"ok", false}, {"error", "old result"}}));
        QCOMPARE(view.request().at("x"), Json(8));
        QCOMPARE(view.request().at("sample"), Json(1));
    }
    void workspaceAndBackendFailure() {
        QTemporaryDir dir;
        fixture().save(dir.filePath("frame.gpa_frame"));
        MainWindow window;
        window.show();
        auto tabs = window.findChild<QTabWidget *>("analysisTabs");
        QVERIFY(tabs);
        QStringList names;
        for (int i = 0; i < tabs->count(); ++i)
            names << tabs->tabText(i);
        QVERIFY(names.contains("Resources"));
        QVERIFY(!names.contains("Output"));
        QVERIFY(!names.contains("Texture"));
        QVERIFY(!names.contains("Coverage"));
        QVERIFY(!names.contains("Pixel History"));
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(dir.filePath("frame.gpa_frame"));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<ResourceBrowser *>()->bindings().empty(), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.back()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto tree = window.findChild<QTreeWidget *>("drawResources");
        QVERIFY(tree);
        QCOMPARE(window.findChild<ResourceBrowser *>()->selected()->kind, std::string("RTV"));
        auto history = window.findChild<PixelHistoryView *>();
        history->setBackendPath("missing-renderdoc.dll");
        window.findChild<QAction *>("pickPixelHistory")->setChecked(true);
        auto image = window.findChild<ImageView *>("frameOutput");
        QTRY_VERIFY_WITH_TIMEOUT(!image->image().isNull(), 30000);
        QSignalSpy pixels(image, &ImageView::pixelSelected);
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(.5, .5)));
        QCOMPARE(pixels.count(), 1);
        QTRY_VERIFY(window.findChild<QLabel *>("historySummary")->text().contains("1.45"));
        window.findChild<QAction *>("pickPixelHistory")->setChecked(false);
        window.findChild<QAction *>("toggleResourceCoverage")->setChecked(true);
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 30000);
        auto browser = window.findChild<ResourceBrowser *>();
        QVERIFY(browser->select("in/PS/SRV/0", true));
        QVERIFY(!image->hasOverlay());
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(browser->select("out/OM/DSV/0", true));
        QVERIFY(browser->select("out/OM/RTV/0", true));
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 30000);
        QCOMPARE(browser->selected()->image.resource, Id(20));
        window.findChild<QAction *>("toggleResourceCoverage")->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(window.findChild<QWidget *>("coverageView"), "diagnosticRequested"));
        QVERIFY(!browser->selected());
        QVERIFY(!window.findChild<QAction *>("importTextureInput")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("exportOutputStorage")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("exportTexture")->isEnabled());
        QVERIFY(!image->image().isNull()); // The diagnostic still supports PNG export.
        tasks.clear();
        QVERIFY(browser->select("out/OM/RTV/0", true));
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QVERIFY(browser->selected());
        QVERIFY(window.findChild<QAction *>("exportOutputStorage")->isEnabled());
    }
    void directResourceDoesNotChooseAnAlias() {
        QTemporaryDir dir;
        fixture().save(dir.filePath("frame.gpa_frame"));
        MainWindow window;
        window.show();
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(dir.filePath("frame.gpa_frame"));
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        tasks.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row)
            if (api->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 1000)
                api->setCurrentIndex(api->model()->index(row, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        auto browser = window.findChild<ResourceBrowser *>();
        QVERIFY(browser->selected());
        QCOMPARE(browser->selected()->kind, std::string("RTV"));
        size_t aliases = 0;
        for (const auto &binding : browser->bindings())
            if (binding.image.resource == 20) ++aliases;
        QCOMPARE(aliases, size_t(2)); // Same storage has both RTV and SRV identities.
        auto resources = window.findChild<QTableView *>("resources");
        tasks.clear();
        for (int row = 0; row < resources->model()->rowCount(); ++row)
            if (resources->model()->index(row, 0).data(Qt::UserRole).toULongLong() == 20)
                resources->setCurrentIndex(resources->model()->index(row, 0));
        QVERIFY(!browser->selected());
        auto status = window.findChild<QLabel *>("resourceStatus");
        QVERIFY(status->text().contains("Texture 20"));
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(window.findChild<QComboBox *>("textureBoundary")->currentIndex(), 2);
        tasks.clear();
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QVERIFY(!browser->selected());
        QVERIFY(!status->text().contains("RTV") && !status->text().contains("SRV"));
        tasks.clear();
        QVERIFY(browser->select("in/PS/SRV/0", true));
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(browser->selected()->kind, std::string("SRV"));
        QVERIFY(status->text().contains("PS SRV0"));
        const auto rows = browser->bindings();
        const auto context = browser->contextKey();
        auto unavailable = *browser->selected();
        unavailable.error = "Unavailable binding control";
        browser->setContext("unavailable", {unavailable}, 1000);
        QVERIFY(browser->select(unavailable.key, true));
        QCOMPARE(status->text(), QString("Unavailable binding control"));
        QVERIFY(window.findChild<ImageView *>("frameOutput")->image().isNull());
        QVERIFY(window.findChild<ImageView *>("textureOutput")->image().isNull());
        QVERIFY(!window.findChild<QAction *>("importTextureInput")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("exportTexture")->isEnabled());
        browser->setContext(context, rows, 1000);
        tasks.clear();
        QVERIFY(browser->select("out/OM/RTV/0", true));
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(browser->selected()->kind, std::string("RTV"));
        QVERIFY(!window.findChild<ImageView *>("frameOutput")->image().isNull());
        QVERIFY(window.findChild<QAction *>("importTextureInput")->isEnabled());
    }
    void realCaptureScreenshots() {
        const auto capture = qEnvironmentVariable("FLORA_RESOURCE_CAPTURE");
        const auto output = qEnvironmentVariable("FLORA_RESOURCE_SCREENSHOTS");
        if (capture.isEmpty() || output.isEmpty())
            QSKIP("Set capture and screenshot output to exercise a real frame");
        QVERIFY(QDir().mkpath(output));
        Frame frame(capture.toStdWString());
        Id selected = 0;
        for (auto id : frame.entryOrder()) {
            const auto &e = frame.entry(id);
            if (e.category != 7 || e.type < 0x37 || e.type > 0x3d)
                continue;
            auto s = frame.state(frame.event(id).state);
            unsigned inputs = 0;
            for (auto srv : s.stages[4].srv)
                inputs += srv != 0;
            if (inputs >= 3 && std::min(s.omStart, s.rtCount) >= 3) {
                selected = id;
                break;
            }
        }
        QVERIFY(selected);
        MainWindow window;
        window.resize(1800, 1000);
        window.show();
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.back()[0].toBool());
        tasks.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == selected) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY2(tasks.back()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QTest::qWait(500);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 60000);
        auto tree = window.findChild<QTreeWidget *>("drawResources");
        QVERIFY(tree);
        QVERIFY(window.grab().save(output + "/resources.png"));
        auto browser = window.findChild<ResourceBrowser *>();
        auto input = std::find_if(browser->bindings().begin(), browser->bindings().end(), [](const auto &b) {
            return b.role == ResourceRole::Input && b.texture && b.error.empty();
        });
        QVERIFY(input != browser->bindings().end());
        tasks.clear();
        browser->select(input->key, true);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.back()[0].toBool());
        QVERIFY(window.grab().save(output + "/input.png"));
        tasks.clear();
        browser->select("out/OM/RTV/1", true);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
        QVERIFY(tasks.back()[0].toBool());
        window.findChild<QAction *>("toggleResourceCoverage")->setChecked(true);
        auto image = window.findChild<ImageView *>("frameOutput");
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 60000);
        QVERIFY(window.grab().save(output + "/coverage.png"));
        // A later BF1 draw has nonzero depth-tested coverage (the early MRT draw may have none).
        tasks.clear();
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 30072) {
                api->setCurrentIndex(api->model()->index(i, 0));
                break;
            }
        QTRY_VERIFY_WITH_TIMEOUT(image->hasOverlay(), 60000);
        QTest::qWait(400);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 60000);
        QVERIFY(window.grab().save(output + "/coverage-visible.png"));
        window.findChild<QAction *>("pickPixelHistory")->setChecked(true);
        window.findChild<PixelHistoryView *>()->setBackendPath("missing-renderdoc.dll");
        QTest::mouseClick(image->viewport(), Qt::LeftButton, Qt::NoModifier,
                          image->mapFromScene(QPointF(100.5, 100.5)));
        QTRY_VERIFY(window.findChild<QLabel *>("historySummary")->text().contains("1.45"));
        QVERIFY(window.grab().save(output + "/history.png"));
    }
};
QTEST_MAIN(DrawResourceTests)
#include "DrawResourceTests.moc"
