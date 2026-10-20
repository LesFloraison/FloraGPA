#include "application/ReplaySession.h"
#include "application/Experiment.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
#include "dx12/SubmissionIndex.h"
#include "dx12/QueryReplayIndex.h"
#include "dx12/QueueDependencyIndex.h"
#include "dx12/InitializationCopy.h"
#include "dx12/PipelineRecipes.h"
#include "dx12/CapturedObjectMarker.h"
#include "dx12/NativeResources.h"
#include "dx12/TileRecipes.h"
#include "dx12/NativeReplay.h"
#include "dx12/NativeTimeline.h"
#include "dx12/NativeCommands.h"
#include "dx12/AddressIndex.h"
#include "dx12/DescriptorRecipes.h"
#include "dx12/NativeDescriptors.h"
#include "dx12/BackBufferIndex.h"
#include "app/MainWindow.h"
#include "app/Appearance.h"
#include "app/Models.h"
#include "app/Views.h"
#include "app/OutputStorageExport.h"
#include "app/WorkerImage.h"
#include "app/ReplayDebugView.h"
#include "app/ReplayMeshView.h"
#include "app/PixelHistoryView.h"
#include "app/ResourceHistoryView.h"
#include "app/Dx12PipelineView.h"
#include "app/RdcCountersView.h"
#include "application/NativeDebugConfig.h"
#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QtTest>
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <bit>

using namespace flora;
namespace {
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot create test input");
}
void qword(QByteArray &bytes, uint64_t value) { bytes.append(reinterpret_cast<const char *>(&value), 8); }
void dword(QByteArray &bytes, uint32_t value) { bytes.append(reinterpret_cast<const char *>(&value), 4); }
QString fixture(QTemporaryDir &directory) {
    const auto path = directory.path() + "/frame.gpa_frame";
    QDir().mkpath(path + "/D3D12");
    QByteArray header(0x288, 0);
    const uint64_t type=10, magic=0x415047;
    const uint32_t version=3;
    memcpy(header.data(), &type, 8); memcpy(header.data()+8, &magic, 8); memcpy(header.data()+16,&version,4);
    write(path+"/metadata",header); write(path+"/resources", "resource");
    QByteArray packets;
    for (uint64_t timestamp : {10ULL,20ULL}) {
        qword(packets, 0x1ae3c86b1c3b9ULL); qword(packets,timestamp); qword(packets,0); qword(packets,28);
        qword(packets,0x123456789abcdef0ULL);
        const std::array<uint32_t,5> args{36,2,0,uint32_t(-3),0};
        packets.append(reinterpret_cast<const char *>(args.data()),20);
    }
    write(path+"/thread-7-0",packets);
    return path;
}
}
class Dx12SessionTests : public QObject {
    Q_OBJECT
  private slots:
    void pipelineShaderNavigation() {
        using Json = nlohmann::json;
        Dx12PipelineView view;
        QSignalSpy creation(&view, &Dx12PipelineView::creationRequested);
        auto rows = view.findChild<QTreeWidget *>("dx12PipelineRows");
        auto replace = view.findChild<QAction *>("replaceBoundPipelineShader");
        Json report{{"ok", true}, {"backend", "dx12"}, {"execution", "1"},
            {"pipeline_scope", Json::object()}, {"pipeline_rows", Json::array()}};
        for (const auto id : {9007199254740993ULL, 9007199254740994ULL}) {
            view.setContext(QString::number(id), 1);
            const Json owner{{"mapping_status", "capture_object_marker"}, {"object_kind", "pipeline"},
                {"object_creation", std::to_string(id)}, {"captured_object", "18446744073709551615"}};
            // A shared RenderDoc shader ID must resolve through this bound PSO,
            // not through a shader-ID-only cache from a different pipeline.
            report["pipeline_rows"] = Json::array({{{"section", "VS"}, {"binding", "Shader"},
                {"details", Json::object()}, {"resource", {{"mapping_status", "capture_pipeline_stage"},
                    {"name", "Shared shader"}, {"rdc_id", "ResourceId::50"}, {"shader_stage", "VS"},
                    {"resource_creation", nullptr}, {"resource_object", nullptr}, {"pipeline", owner}}}}});
            QVERIFY(view.finish(view.requestId(), report)); rows->setCurrentItem(rows->topLevelItem(0));
            QVERIFY(replace->isEnabled()); QVERIFY(!view.findChild<QAction *>("inspectPipelineResource")->isEnabled());
            view.findChild<QAction *>("locatePipelineCreation")->trigger();
            QCOMPARE(creation.takeFirst()[0].toULongLong(), id);
            auto invalid = report; invalid["pipeline_rows"][0]["resource"]["shader_stage"] = "PS";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), invalid));
            invalid = report; invalid["pipeline_rows"][0]["resource"]["pipeline"]["object_creation"] = id;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), invalid));
            QCOMPARE(view.result(), report);
            view.setWorkerBusy(true); QVERIFY(!replace->isEnabled()); view.setWorkerBusy(false);
        }
        const auto old = view.requestId(); view.setContext("changed-experiment", 1);
        QVERIFY(!replace->isEnabled()); QVERIFY(!view.finish(old, report));
        // A modal chooser may outlive the selected session/experiment. Its old
        // target must not be submitted after that context is invalidated.
        QVERIFY(view.finish(view.requestId(), report)); rows->setCurrentItem(rows->topLevelItem(0));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        QTemporaryDir temp; const auto path = temp.path() + "/shader.dxbc"; write(path, "fixture");
        QSignalSpy imports(&view, &Dx12PipelineView::shaderReplacementRequested);
        QTimer chooser, watchdog; watchdog.setSingleShot(true); bool chosen{};
        connect(&chooser, &QTimer::timeout, &view, [&] {
            if (auto dialog = view.findChild<QFileDialog *>()) {
                chooser.stop(); chosen = true; view.setContext("different-session", 1);
                dialog->findChild<QLineEdit *>("fileNameEdit")->setText(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        connect(&watchdog, &QTimer::timeout, &view, [&] { if (auto dialog = view.findChild<QFileDialog *>()) dialog->reject(); });
        chooser.start(10); watchdog.start(5000); replace->trigger();
        QVERIFY(chosen); QVERIFY(imports.empty());
    }
    void pipelineShaderWorkspace() {
        using Json = nlohmann::json;
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto evidence = qEnvironmentVariable("FLORA_DX12_STAGE_PIPELINE_REPORT");
        const auto replacement = qEnvironmentVariable("FLORA_DX12_STAGE_REPLACEMENT");
        const auto output = qEnvironmentVariable("FLORA_DX12_STAGE_PROJECT_OUTPUT");
        if (root.isEmpty() || evidence.isEmpty() || replacement.isEmpty() || output.isEmpty())
            QSKIP("Set Talos capture, pipeline report, VS replacement and new project output");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        QFile file(evidence); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report = Json::parse(file.readAll().toStdString());
        size_t shaderRow = SIZE_MAX;
        for (size_t i = 0; i < report.at("pipeline_rows").size(); ++i)
            if (report["pipeline_rows"][i]["section"] == "VS" && report["pipeline_rows"][i]["binding"] == "Shader") shaderRow = i;
        QVERIFY(shaderRow != SIZE_MAX);
        const auto &row = report["pipeline_rows"][shaderRow];
        QCOMPARE(row["resource"]["pipeline"]["object_creation"], Json("156847"));
        QCOMPARE(row["resource"]["shader_stage"], Json("VS"));
        const auto session = openReplaySession(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        const auto info = session->eventDetails(156847).at("pipeline");
        bool matched{};
        for (const auto &shader : info.at("shaders")) if (shader.at("stage") == "VS") {
            QCOMPARE(row["details"]["bytecode_sha256"], shader.at("sha256")); matched = true;
        }
        QVERIFY(matched);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window; QSignalSpy loaded(&window, &MainWindow::captureLoaded), prepared(&window, &MainWindow::experimentFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto view = window.findChild<Dx12PipelineView *>(); QVERIFY(view->finish(view->requestId(), report));
        auto rows = view->findChild<QTreeWidget *>("dx12PipelineRows"); rows->setCurrentItem(rows->topLevelItem(int(shaderRow)));
        const auto old = view->requestId();
        QTimer chooser, watchdog; watchdog.setSingleShot(true); bool chosen{};
        connect(&chooser, &QTimer::timeout, &window, [&] {
            if (auto dialog = view->findChild<QFileDialog *>()) {
                chooser.stop(); chosen = true;
                dialog->findChild<QLineEdit *>("fileNameEdit")->setText(replacement);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        connect(&watchdog, &QTimer::timeout, &window, [&] { if (auto dialog = view->findChild<QFileDialog *>()) dialog->reject(); });
        chooser.start(10); watchdog.start(5000); view->findChild<QAction *>("replaceBoundPipelineShader")->trigger();
        QVERIFY(chosen); QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeFirst()[0].toBool());
        QVERIFY(view->result().is_null()); QVERIFY(!view->finish(old, report));
        QVERIFY(window.saveDx12Experiment(output + "/replaced.json"));
        QFile project(output + "/replaced.json"); QVERIFY(project.open(QIODevice::ReadOnly));
        const auto document = Json::parse(project.readAll().toStdString());
        const auto &op = document.at("history").at(0).at("operations").at(0);
        QCOMPARE(op.at("pipeline"), Json("156847")); QCOMPARE(op.at("stage"), Json("VS"));
        QCOMPARE(op.at("original_sha256"), row["details"]["bytecode_sha256"]);
        QVERIFY(op.at("asset").at("sha256") != op.at("original_sha256"));
    }
    void pipelineObjectMarkers() {
        for (const auto kind : {dx12::CapturedObjectKind::Pipeline, dx12::CapturedObjectKind::RootSignature}) {
            const auto name = dx12::capturedObjectMarker(kind, UINT64_MAX, 9007199254740993ULL);
            const auto parsed = dx12::parseCapturedObjectMarker(name);
            QVERIFY(parsed); QCOMPARE(parsed->kind, kind);
            QCOMPARE(parsed->object, UINT64_MAX); QCOMPARE(parsed->creation, 9007199254740993ULL);
            QVERIFY(!dx12::parseCapturedObjectMarker(name + " extra"));
        }
        for (const auto name : {"Flora DX12 pipeline 1 creation 0", "Flora DX12 pipeline 01 creation 2",
                "Flora DX12 pipeline 18446744073709551616 creation 2", "Flora DX12 root signature 1 creation -2",
                "Flora DX12 resource 1 creation 2", "Flora DX12 pipeline 1", ""})
            QVERIFY(!dx12::parseCapturedObjectMarker(name));
    }
    void pipelineObjectNavigation() {
        using Json = nlohmann::json;
        Dx12PipelineView view; view.setContext("pipeline-session", 12675);
        Json report{{"ok", true}, {"backend", "dx12"}, {"execution", "12675"},
            {"pipeline_scope", Json::object()}, {"pipeline_rows", Json::array()}};
        for (const auto kind : {"pipeline", "root_signature"})
            report["pipeline_rows"].push_back({{"section", "Pipeline"}, {"binding", kind}, {"details", Json::object()},
                {"resource", {{"name", kind}, {"rdc_id", "ResourceId::1"}, {"mapping_status", "capture_object_marker"},
                    {"object_kind", kind}, {"captured_object", "18446744073709551615"},
                    {"object_creation", "9007199254740993"}, {"resource_creation", nullptr}, {"resource_object", nullptr}}}});
        QSignalSpy creation(&view, &Dx12PipelineView::creationRequested), resource(&view, &Dx12PipelineView::resourceRequested);
        auto rows = view.findChild<QTreeWidget *>("dx12PipelineRows");
        auto locate = view.findChild<QAction *>("locatePipelineCreation");
        QVERIFY(view.finish(view.requestId(), report));
        for (int i = 0; i < 2; ++i) {
            rows->setCurrentItem(rows->topLevelItem(i));
            QVERIFY(locate->isEnabled()); QVERIFY(!view.findChild<QAction *>("inspectPipelineResource")->isEnabled());
            locate->trigger(); QCOMPARE(creation.takeFirst()[0].toULongLong(), 9007199254740993ULL);
        }
        QVERIFY(resource.empty());
        view.setWorkerBusy(true); QVERIFY(!locate->isEnabled()); view.setWorkerBusy(false);
        auto invalid = report; invalid["pipeline_rows"][0]["resource"]["object_creation"] = 9007199254740993ULL;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), invalid));
        QCOMPARE(view.result(), report);
        auto legacy = report;
        for (auto &row : legacy["pipeline_rows"]) {
            auto &ref = row["resource"]; ref["mapping_status"] = "unmapped";
            ref.erase("object_creation"); ref.erase("captured_object"); ref.erase("object_kind");
        }
        QVERIFY(view.finish(view.requestId(), legacy));
        rows->setCurrentItem(rows->topLevelItem(0)); QVERIFY(!locate->isEnabled());
        const auto old = view.requestId(); view.setContext("changed-experiment", 12675);
        QVERIFY(!view.finish(old, report)); QVERIFY(!locate->isEnabled());
    }
    void pipelineObjectWorkspace() {
        using Json = nlohmann::json;
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto evidence = qEnvironmentVariable("FLORA_DX12_OBJECT_PIPELINE_REPORT");
        if (root.isEmpty() || evidence.isEmpty()) QSKIP("Set Talos capture and marked pipeline report");
        QFile file(evidence); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report = Json::parse(file.readAll().toStdString());
        QVERIFY(report.at("ok").get<bool>()); QCOMPARE(report.at("execution"), Json("12675"));
        const auto path = std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString());
        dx12::StreamDirectory directory(path);
        MainWindow window; QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        window.openCapture(QString::fromStdWString(path.wstring()));
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        auto view = window.findChild<Dx12PipelineView *>();
        auto domain = window.findChild<QComboBox *>("apiEventDomain");
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        size_t checked{};
        for (size_t i = 0; i < report.at("pipeline_rows").size(); ++i) {
            const auto &row = report.at("pipeline_rows")[i];
            if (row.at("section") != "Pipeline") continue;
            const auto &ref = row.at("resource");
            QCOMPARE(ref.at("mapping_status"), Json("capture_object_marker"));
            const auto record = std::stoull(ref.at("object_creation").get<std::string>());
            const auto object = std::stoull(ref.at("captured_object").get<std::string>());
            const auto &packet = directory.packets().at(size_t(record - 1));
            if (ref.at("object_kind") == "pipeline")
                QCOMPARE(dx12::decodePipelineRecipe(packet.type, directory.payload(packet)).object, object);
            else {
                QCOMPARE(packet.type, uint64_t(0x127938051b445ULL));
                QCOMPARE(dx12::decodeRootSignature(directory.payload(packet)).object, object);
            }
            domain->setCurrentIndex(1); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
            QVERIFY(view->finish(view->requestId(), report));
            auto rows = view->findChild<QTreeWidget *>("dx12PipelineRows");
            rows->setCurrentItem(rows->topLevelItem(int(i)));
            window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
            view->findChild<QAction *>("locatePipelineCreation")->trigger();
            QCOMPARE(domain->currentIndex(), 0);
            QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(), record);
            QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
            QVERIFY(view->result().is_null()); ++checked;
        }
        QCOMPARE(checked, size_t(2));
    }
    void swapchainPacketArguments() {
        using Json = nlohmann::json;
        const uint64_t swapchain = UINT64_MAX - 5, resource = UINT64_MAX - 6;
        const auto decode = [](uint64_t type, const QByteArray &bytes) {
            return dx12::decodeArguments(type, format::Bytes(
                reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())));
        };
        QByteArray bytes; qword(bytes, swapchain); dword(bytes, 1);
        const auto iid = __uuidof(ID3D12Resource);
        bytes.append(reinterpret_cast<const char *>(&iid), sizeof(iid)); bytes.append(char(1));
        qword(bytes, 1); qword(bytes, resource); dword(bytes, 0);
        const auto buffer = decode(0x1d532a4b90915ULL, bytes);
        QVERIFY(buffer.supported); QCOMPARE(buffer.consumed, size_t(bytes.size()));
        QCOMPARE(buffer.values.at("SwapChain"), Json(std::to_string(swapchain)));
        QCOMPARE(buffer.values.at("Resource"), Json(std::to_string(resource)));
        QCOMPARE(buffer.values.at("BufferIndex"), Json(1));
        QCOMPARE(buffer.values.at("InterfaceId"), Json("696442be-a72e-4059-bc79-5b5c98040fad"));
        QCOMPARE(buffer.resources, std::vector<uint64_t>{resource});
        bytes.chop(1); QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode(0x1d532a4b90915ULL, bytes));
        bytes.clear(); qword(bytes, swapchain); dword(bytes, 3);
        const auto current = decode(0x1af998824f46bULL, bytes);
        QCOMPARE(current.values.at("ReturnedIndex"), Json(3));
        QVERIFY(current.resources.empty());
        bytes.append(char(0)); QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode(0x1af998824f46bULL, bytes));
        bytes.clear(); qword(bytes, swapchain); dword(bytes, 0); dword(bytes, 512); dword(bytes, 0);
        const auto present = decode(0x1d53218275f72ULL, bytes);
        QCOMPARE(present.values.at("SyncInterval"), Json(0));
        QCOMPARE(present.values.at("PresentFlags"), Json(512));
        QCOMPARE(present.values.at("HRESULT"), Json(0));
        bytes.chop(1); QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode(0x1d53218275f72ULL, bytes));
    }
    void hellbladePresentIndexDomains() {
        using Json = nlohmann::json;
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set captures for the Hellblade2 presentation-index evidence");
        auto session = openReplaySession(std::filesystem::path(
            (root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString()));
        const auto buffer = session->eventDetails(506294).at("arguments");
        const auto current = session->eventDetails(613348).at("arguments");
        const auto present = session->eventDetails(613510).at("arguments");
        const auto copy = session->eventDetails(613353).at("arguments");
        QCOMPARE(buffer.at("BufferIndex"), Json(1));
        QCOMPARE(current.at("ReturnedIndex"), Json(3));
        QCOMPARE(buffer.at("SwapChain"), current.at("SwapChain"));
        QCOMPARE(buffer.at("SwapChain"), present.at("SwapChain"));
        QCOMPARE(buffer.at("Resource"), copy.at("Destination"));
        QCOMPARE(buffer.at("Resource"), Json("2149283441408"));
        QCOMPARE(session->eventDetails(506294).at("backbuffer_index_domain"), Json("serialized_getbuffer_index"));
        QCOMPARE(session->eventDetails(613348).at("backbuffer_index_domain"), Json("captured_current_index_observation"));
        QCOMPARE(session->resourceDetails(506294).at("swapchain_buffer").at("index_domain"), Json("serialized_getbuffer_index"));
        QVERIFY(QString::fromStdString(session->resource(506294).name).contains("serialized index"));
        QCOMPARE(present.at("PresentFlags"), Json(512));
        QCOMPARE(present.at("HRESULT"), Json(0));
        QVERIFY(QString::fromStdString(session->argumentText(506294)).contains("2149283441408"));
        QVERIFY(QString::fromStdString(session->argumentText(613348)).contains("ReturnedIndex"));
    }
    void nativeQueuedSubmissions() {
        using Json = nlohmann::json;
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set captures to select the Talos SDK/device for the queue fixture");
        dx12::StreamDirectory sdk(
            std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        QTemporaryDir temp;
        const auto path = fixture(temp);
        QByteArray packets;
        uint64_t record{};
        const auto packet = [&](uint64_t type, const QByteArray &bytes) {
            qword(packets, type);
            qword(packets, ++record);
            qword(packets, 0);
            qword(packets, bytes.size());
            packets += bytes;
        };
        const uint64_t producer = 0x123456789abcde01ULL, consumer = producer + 1, fence = producer + 2,
                       allocator = 43;
        const auto output = [&](QByteArray &bytes, const GUID &iid, uint64_t object) {
            bytes.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
            bytes.append(char(1));
            qword(bytes, 1);
            qword(bytes, object);
            dword(bytes, 0);
        };
        for (const auto queue : {producer, consumer}) {
            QByteArray bytes;
            qword(bytes, 1);
            bytes.append(char(1));
            D3D12_COMMAND_QUEUE_DESC desc{};
            bytes.append(reinterpret_cast<const char *>(&desc), sizeof(desc));
            output(bytes, __uuidof(ID3D12CommandQueue), queue);
            packet(0x127932607a959ULL, bytes);
        }
        QByteArray bytes;
        qword(bytes, 1);
        dword(bytes, D3D12_COMMAND_LIST_TYPE_DIRECT);
        output(bytes, __uuidof(ID3D12CommandAllocator), allocator);
        packet(0x12793beef5413ULL, bytes);
        bytes.clear();
        qword(bytes, 1);
        qword(bytes, 0);
        dword(bytes, 0);
        output(bytes, __uuidof(ID3D12Fence), fence);
        packet(0x127930cf4f3a0ULL, bytes);
        const uint64_t texture = 60;
        bytes.clear(); qword(bytes, 1); bytes.append(char(1));
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        bytes.append(reinterpret_cast<const char *>(&properties), sizeof(properties));
        dword(bytes, 0); bytes.append(char(1));
        D3D12_RESOURCE_DESC description{}; description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = description.Height = 4; description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        bytes.append(reinterpret_cast<const char *>(&description), sizeof(description));
        dword(bytes, D3D12_RESOURCE_STATE_COPY_DEST); bytes.append(char(0));
        output(bytes, __uuidof(ID3D12Resource), texture); packet(0x12793f58d8f79ULL, bytes);
        const auto list = [&](uint64_t object, bool creation = false) {
            QByteArray b;
            if (creation) {
                qword(b, 1);
                dword(b, 0);
                dword(b, D3D12_COMMAND_LIST_TYPE_DIRECT);
                qword(b, allocator);
                qword(b, 0);
                output(b, __uuidof(ID3D12GraphicsCommandList), object);
                packet(0x127938b682074ULL, b);
            } else {
                qword(b, object);
                qword(b, allocator);
                qword(b, 0);
                dword(b, 0);
                packet(0x1ae3c721c351aULL, b);
            }
            b.clear();
            qword(b, object);
            const auto barriers = object == 51 ? 2U : object == 52 ? 1U : 0U;
            dword(b, barriers); b.append(char(barriers != 0));
            if (barriers) {
                qword(b, barriers);
                const auto transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
                    dword(b, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION); dword(b, 0); qword(b, texture);
                    dword(b, 0); dword(b, before); dword(b, after); dword(b, 0);
                };
                if (object == 52) transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
                else {
                    transition(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
            packet(0x1ae3c24e5dabeULL, b);
            b.clear();
            qword(b, object);
            dword(b, 0);
            packet(0x1ae3cd6acf667ULL, b);
        };
        const auto submit = [&](uint64_t queue, uint64_t object) {
            QByteArray b;
            qword(b, queue);
            dword(b, 1);
            b.append(char(1));
            qword(b, 1);
            qword(b, object);
            packet(0x125482975df48ULL, b);
        };
        const auto sync = [&](bool wait, uint64_t queue) {
            QByteArray b;
            qword(b, queue);
            qword(b, fence);
            qword(b, 1);
            dword(b, 0);
            packet(wait ? 0x12548db3bfde2ULL : 0x12548dab1bf2dULL, b);
        };
        list(50, true);
        submit(producer, 50);
        const auto initializationCount = record;
        QFile headerFile(path + "/metadata");
        QVERIFY(headerFile.open(QIODevice::ReadOnly));
        auto metadata = headerFile.readAll();
        headerFile.close();
        QByteArray section;
        qword(section, 3);
        qword(section, 0);
        qword(section, 0);
        qword(section, uint64_t(metadata.size() + 40 + packets.size()));
        dword(section, 0);
        dword(section, 0);
        metadata += section + packets;
        write(path + "/metadata", metadata);
        packets.clear();
        list(51);
        list(52);
        sync(true, consumer);
        const auto waitRecord = record;
        bytes.clear();
        qword(bytes, fence);
        qword(bytes, 0);
        packet(0x11469e993999dULL, bytes);
        submit(consumer, 51);
        const auto consumerRecord = record;
        submit(producer, 52);
        const auto producerRecord = record;
        sync(false, producer);
        const auto signalRecord = record;
        bytes.clear();
        qword(bytes, fence);
        qword(bytes, 1);
        packet(0x11469e993999dULL, bytes);
        submit(consumer, 51);
        const auto repeatedRecord = record;
        write(path + "/thread-7-0", packets);
        dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
        QTemporaryDir conflicting;
        const auto conflictPath = fixture(conflicting);
        write(conflictPath + "/metadata", metadata);
        packets.clear();
        record = initializationCount;
        list(51);
        list(52);
        sync(true, consumer);
        bytes.clear();
        qword(bytes, fence);
        qword(bytes, 1);
        qword(bytes, 0);
        dword(bytes, 0);
        packet(0x114699e3dba70ULL, bytes);
        submit(consumer, 51);
        submit(producer, 52);
        sync(false, producer);
        submit(consumer, 51);
        write(conflictPath + "/thread-7-0", packets);
        dx12::StreamDirectory conflictDirectory(std::filesystem::path(conflictPath.toStdWString()));
        Json evidence = Json::array();
        // CPU-order state validation observes COPY_DEST at the consumer's
        // RENDER_TARGET barrier. Dependency-ready validation must run the
        // producer first. Cancellation stops after issuing that producer.
        for (const bool interrupt : {false, true, false}) {
            try {
                dx12::NativeDevice device(sdk, true);
                dx12::NativeReplay conflict(device, conflictDirectory, 4ULL * 1024 * 1024);
                try {
                    conflict.prepareExecution(4, true);
                    QFAIL("Host drain before the future producer was accepted");
                } catch (const std::runtime_error &error) {
                    QVERIFY(QString::fromUtf8(error.what()).contains("beyond host drain"));
                }
                QCOMPARE(conflict.information().at("frame_queue_schedule").at("queued_submissions"),
                         Json("0"));
                bool armed{};
                dx12::NativeReplay *active{};
                dx12::NativeReplay replay(device, directory, 4ULL * 1024 * 1024, [&] {
                    return armed && interrupt &&
                           active->information().at("frame_queue_schedule").at("queued_submissions") == "1";
                });
                active = &replay;
                replay.prepareExecution(4, true);
                replay.initializeTransfers();
                armed = true;
                if (interrupt) {
                    QVERIFY_THROWS_EXCEPTION(OperationCancelled, replay.replayTo(4, true));
                    const auto report = replay.information();
                    const auto &schedule = report.at("frame_queue_schedule");
                    QVERIFY(schedule.at("aborted_device").get<bool>());
                    QCOMPARE(schedule.at("queued_submissions"), Json("1"));
                    QCOMPARE(schedule.at("completed_submissions"), Json("0"));
                    QCOMPARE(schedule.at("pending_submissions"), Json("1"));
                    QCOMPARE(schedule.at("pending_queue_operations"), Json("2"));
                    QVERIFY(!report.at("execution_boundary_reached").get<bool>());
                    QVERIFY(FAILED(device.device()->GetDeviceRemovedReason()));
                    evidence.push_back(report);
                } else {
                    replay.replayTo(4, true);
                    const auto report = replay.information();
                    const auto &schedule = report.at("frame_queue_schedule");
                    QCOMPARE(schedule.at("queued_submissions"), Json("3"));
                    QCOMPARE(schedule.at("completed_submissions"), Json("3"));
                    QCOMPARE(schedule.at("pending_submissions"), Json("0"));
                    QCOMPARE(schedule.at("maximum_host_pending_submissions"), Json("3"));
                    QCOMPARE(schedule.at("host_drains"), Json("1"));
                    QCOMPARE(schedule.at("fence_observations"), Json("2"));
                    QCOMPARE(schedule.at("deferred_queue_operations"), Json("2"));
                    QCOMPARE(schedule.at("pending_queue_operations"), Json("0"));
                    const auto &issued = schedule.at("first_issued_queue_operations");
                    QCOMPARE(issued.size(), size_t(5));
                    const std::array<uint64_t, 5> expected{producerRecord, signalRecord, waitRecord,
                                                          consumerRecord, repeatedRecord};
                    for (size_t i = 0; i < expected.size(); ++i)
                        QCOMPARE(issued[i].at("record"), Json(std::to_string(expected[i])));
                    QCOMPARE(schedule.at("dependencies").at("later_signal_waits"), Json("1"));
                    QVERIFY(!schedule.at("aborted_device").get<bool>());
                    QVERIFY(report.at("execution_boundary_reached").get<bool>());
                    QCOMPARE(device.information().at("validation_error_count"), Json("0"));
                    evidence.push_back(report);
                }
            } catch (const std::exception &error) {
                QFAIL(error.what());
            }
        }
        const auto out = qEnvironmentVariable("FLORA_DX12_QUEUE_EVIDENCE");
        if (!out.isEmpty())
            write(out, QByteArray::fromStdString(evidence.dump(2)));
    }
    void queueDependencies() {
        using Json = nlohmann::json;
        QTemporaryDir temp;
        const auto path = fixture(temp);
        QByteArray packets;
        uint64_t record{};
        const auto packet = [&](uint64_t type, const QByteArray &bytes) {
            qword(packets, type);
            qword(packets, ++record);
            qword(packets, 0);
            qword(packets, bytes.size());
            packets += bytes;
            return record;
        };
        const uint64_t fence = UINT64_MAX - 10, queue = UINT64_MAX - 20;
        const auto create = [&](uint64_t object, uint64_t initial) {
            QByteArray bytes;
            qword(bytes, 1);
            qword(bytes, initial);
            dword(bytes, 0);
            const auto iid = __uuidof(ID3D12Fence);
            bytes.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
            bytes.append(char(1));
            qword(bytes, 1);
            qword(bytes, object);
            dword(bytes, 0);
            return packet(0x127930cf4f3a0ULL, bytes);
        };
        const auto sync = [&](bool wait, uint64_t q, uint64_t f, uint64_t value) {
            QByteArray bytes;
            qword(bytes, q);
            qword(bytes, f);
            qword(bytes, value);
            dword(bytes, 0);
            return packet(wait ? 0x12548db3bfde2ULL : 0x12548dab1bf2dULL, bytes);
        };
        create(fence, 5);
        sync(true, queue, fence, 5); // Already satisfied, no artificial producer edge.
        const auto producer = sync(false, queue + 1, fence, 6);
        sync(true, queue, fence, 6);
        const auto futureWait = sync(true, queue, fence, 7);
        const auto futureSignal = sync(false, queue + 1, fence, 7);
        write(path + "/thread-7-0", packets);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const dx12::QueueDependencyIndex dependencies(directory);
            const auto report = dependencies.information();
            QCOMPARE(dependencies.observationDependency(fence, 5, futureWait).at("status"), Json("resolved"));
            const auto premature = dependencies.observationDependency(fence, 7, futureWait);
            QCOMPARE(premature.at("status"), Json("unresolved"));
            QCOMPARE(premature.at("required_record"), Json(std::to_string(futureSignal)));
            QCOMPARE(dependencies.observationDependency(fence, 7, futureSignal).at("status"), Json("resolved"));
            QCOMPARE(report.at("unresolved_waits"), Json("0"));
            QCOMPARE(report.at("initial_value_waits"), Json("1"));
            QCOMPARE(report.at("prior_signal_waits"), Json("1"));
            QCOMPARE(report.at("later_signal_waits"), Json("1"));
            QCOMPARE(report.at("waits")[1].at("producer_record"), Json(std::to_string(producer)));
            QCOMPARE(report.at("waits")[2].at("producer_record"), Json(std::to_string(futureSignal)));
            QCOMPARE(report.at("waits")[2].at("fence"), Json(std::to_string(fence)));
            const auto prefix = dx12::QueueDependencyIndex(directory, futureWait).information();
            QCOMPARE(prefix.at("unresolved_waits"), Json("1"));
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     dx12::QueueDependencyIndex(directory, 0, [] { return true; }));
            QCOMPARE(openReplaySession(std::filesystem::path(path.toStdWString()))
                         ->submissionDetails()
                         .at("queue_dependencies"),
                     report);
        }

        // An earlier Signal may itself depend on a producer not submitted yet.
        packets.clear(); record = 0;
        create(fence, 0); create(fence + 1, 0);
        sync(true, queue, fence, 1);
        const auto dependentSignal = sync(false, queue, fence + 1, 1);
        const auto rootSignal = sync(false, queue + 1, fence, 1);
        write(path + "/thread-7-0", packets);
        {
            dx12::StreamDirectory transitive(std::filesystem::path(path.toStdWString()));
            const dx12::QueueDependencyIndex dependencies(transitive);
            const auto early = dependencies.observationDependency(fence + 1, 1, dependentSignal);
            QCOMPARE(early.at("status"), Json("unresolved"));
            QCOMPARE(early.at("required_record"), Json(std::to_string(rootSignal)));
            QCOMPARE(dependencies.observationDependency(fence + 1, 1, rootSignal).at("status"), Json("resolved"));
        }
        // Future producer edges must combine with queue order: this forms a
        // cross-queue cycle, despite both required values appearing in records.
        packets.clear();
        record = 0;
        create(fence, 0);
        create(fence + 1, 0);
        sync(true, queue, fence, 1);
        sync(true, queue + 1, fence + 1, 1);
        sync(false, queue, fence + 1, 1);
        sync(false, queue + 1, fence, 1);
        write(path + "/thread-7-0", packets);
        {
            dx12::StreamDirectory cyclic(std::filesystem::path(path.toStdWString()));
            QCOMPARE(dx12::QueueDependencyIndex(cyclic).information().at("cyclic_or_downstream_waits"),
                     Json("2"));
        }

        // Rewinding or multiple writers cannot be reduced to a maximum value.
        packets.clear();
        record = 0;
        create(fence, 0);
        sync(false, queue, fence, 3);
        sync(false, queue, fence, 1);
        sync(true, queue + 1, fence, 2);
        write(path + "/thread-7-0", packets);
        {
            dx12::StreamDirectory rewound(std::filesystem::path(path.toStdWString()));
            QCOMPARE(dx12::QueueDependencyIndex(rewound).information().at("unresolved_waits"), Json("1"));
        }
        packets.clear();
        record = 0;
        create(fence, 0);
        sync(false, queue, fence, 1);
        sync(false, queue + 1, fence, 2);
        sync(true, queue + 2, fence, 2);
        write(path + "/thread-7-0", packets);
        {
            dx12::StreamDirectory writers(std::filesystem::path(path.toStdWString()));
            QCOMPARE(dx12::QueueDependencyIndex(writers).information().at("waits")[0].at("condition"),
                     Json("unresolved"));
        }
        packets.clear();
        record = 0;
        create(fence, 0);
        create(fence + 1, 0);
        sync(true, queue, fence, 1); // No producer, blocks later operations on this queue.
        sync(false, queue, fence + 1, 1);
        sync(true, queue + 1, fence + 1, 1);
        write(path + "/thread-7-0", packets);
        {
            dx12::StreamDirectory missing(std::filesystem::path(path.toStdWString()));
            const auto unresolved = dx12::QueueDependencyIndex(missing).information();
            QCOMPARE(unresolved.at("unresolved_waits"), Json("2"));
            QCOMPARE(unresolved.at("cyclic_or_downstream_waits"), Json("0"));
            QCOMPARE(unresolved.at("waits")[1].at("condition"), Json("queue_signal"));
            QCOMPARE(unresolved.at("waits")[1].at("dependency_status"), Json("unresolved_or_upstream"));
        }
        packets.clear();
        record = 0;
        create(fence, UINT64_MAX);
        sync(true, queue, fence, 1);
        write(path + "/thread-7-0", packets);
        dx12::StreamDirectory reserved(std::filesystem::path(path.toStdWString()));
        const auto opaque = dx12::QueueDependencyIndex(reserved).information();
        QCOMPARE(opaque.at("invalid_fence_creation_count"), Json("0"));
        QCOMPARE(opaque.at("unresolved_waits"), Json("1"));
    }
    void fixedPipelineComposite() {
        using Json = nlohmann::json;
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto evidence = qEnvironmentVariable("FLORA_DX12_FIXED_PIPELINE_EVIDENCE");
        if (root.isEmpty() || evidence.isEmpty())
            QSKIP("Set BF1 capture and fixed-pipeline evidence directories");
        const auto read = [&](const char *name) {
            QFile file(evidence + '/' + name + "/result.json");
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing fixed-pipeline evidence");
            return Json::parse(file.readAll().toStdString());
        };
        const auto report = read("bf1-color-final-pipeline3");
        const auto compute = read("bf1-compute-fixed-state1");
        const auto history = read("bf1-color-input-pixel-history1");
        for (const auto *r : {&report, &compute, &history}) {
            QVERIFY(r->at("ok").get<bool>());
            QVERIFY(r->at("gpa_modules_loaded").empty());
        }
        const auto detail = [&](const char *binding) -> const Json & {
            for (const auto &row : report.at("pipeline_rows"))
                if (row.at("binding") == binding) return row.at("details");
            throw std::runtime_error("Missing fixed-function pipeline row");
        };
        dx12::StreamDirectory directory(std::filesystem::path(
            (root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        const auto &packet = directory.packets().at(179424 - 1);
        const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
        QCOMPARE(recipe.object, 0x7eb2e6c0ULL);
        const auto &bind = directory.packets().at(213069 - 1);
        format::Reader reader(directory.payload(bind));
        reader.skip(8);
        QCOMPARE(reader.read<uint64_t>(), recipe.object);
        const auto &native = recipe.graphics;
        const auto &b = native.BlendState.RenderTarget[0];
        const auto &blend = detail("Blend target 0");
        QCOMPARE(bool(b.BlendEnable), true);
        QCOMPARE(b.SrcBlend, D3D12_BLEND_ONE);
        QCOMPARE(b.DestBlend, D3D12_BLEND_INV_SRC_ALPHA);
        QCOMPARE(b.BlendOp, D3D12_BLEND_OP_ADD);
        QCOMPARE(blend.at("enabled"), Json(bool(b.BlendEnable)));
        QCOMPARE(blend.at("color").at("source").at("name"), Json("One"));
        QCOMPARE(blend.at("color").at("destination").at("name"), Json("InverseSourceAlpha"));
        QCOMPARE(blend.at("color").at("operation").at("name"), Json("Add"));
        QCOMPARE(blend.at("write_mask"), Json(b.RenderTargetWriteMask));
        QCOMPARE(detail("Blend state").at("independent_blend"), Json(bool(native.BlendState.IndependentBlendEnable)));
        QCOMPARE(detail("Depth / stencil").at("depth_enabled"), Json(bool(native.DepthStencilState.DepthEnable)));
        QCOMPARE(detail("Depth / stencil").at("stencil_enabled"), Json(bool(native.DepthStencilState.StencilEnable)));
        QCOMPARE(detail("State").at("front_counter_clockwise"), Json(bool(native.RasterizerState.FrontCounterClockwise)));
        QCOMPARE(detail("State").at("depth_clip"), Json(bool(native.RasterizerState.DepthClipEnable)));
        QCOMPARE(detail("State").at("sample_mask"), Json(native.SampleMask));
        QCOMPARE(native.InputLayout.NumElements, 2U);
        for (size_t i = 0; i < recipe.inputs.size(); ++i) {
            const auto &l = recipe.inputs[i];
            const auto &row = detail(i == 0 ? "Input 0 TEXCOORD0" : "Input 1 TEXCOORD1");
            QCOMPARE(row.at("semantic"), Json(l.SemanticName));
            QCOMPARE(row.at("semantic_index"), Json(l.SemanticIndex));
            QCOMPARE(row.at("byte_offset"), Json(l.AlignedByteOffset));
            QCOMPARE(row.at("input_slot"), Json(l.InputSlot));
            QCOMPARE(row.at("per_instance"), Json(l.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA));
        }
        QCOMPARE(detail("Viewport 0").at("width"), Json(1920.0));
        QCOMPARE(detail("Viewport 0").at("height"), Json(1009.0));
        QCOMPARE(detail("Scissor 0").at("width"), Json(1920));
        QCOMPARE(detail("Scissor 0").at("height"), Json(1009));
        // Use pre/shader/post values from the same Pixel History record;
        // separately replayed shader traces have different input colors.
        bool verifiedBlend = false;
        for (const auto &row : history.at("history")) {
            if (row.at("execution") != "36640") continue;
            const auto &src = row.at("shaderOut").at("col").at("floatValue");
            const auto &dst = row.at("preMod").at("col").at("floatValue");
            const auto &post = row.at("postMod").at("col").at("floatValue");
            for (size_t c = 0; c < 3; ++c) {
                const float blended = src[c].get<float>() + (1.0f - src[3].get<float>()) * dst[c].get<float>();
                QVERIFY(blended >= 0.03125f && blended < 0.125f);
                const uint32_t mask = UINT32_MAX << (23U - (c == 2 ? 5U : 6U));
                QCOMPARE(Json(std::bit_cast<float>(std::bit_cast<uint32_t>(blended) & mask)), post[c]);
            }
            verifiedBlend = true;
        }
        QVERIFY(verifiedBlend);
        QVERIFY(!compute.at("pipeline_scope").at("graphics_state_applicable").get<bool>());
        for (const auto &row : compute.at("pipeline_rows")) {
            QVERIFY(row.at("binding") != "Blend state");
            QVERIFY(row.at("section") != "Rasterizer");
            QVERIFY(row.at("section") != "Stream output");
        }
        Dx12PipelineView view;
        view.setContext("composite", 36640);
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("dx12PipelineRows");
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->text(1) == "Blend target 0") rows->setCurrentItem(rows->topLevelItem(i));
        QVERIFY(view.findChild<QPlainTextEdit *>()->toPlainText().contains("InverseSourceAlpha"));
        QVERIFY(!view.findChild<QAction *>("inspectPipelineResource")->isEnabled());
        QTemporaryDir temp;
        view.exportResult(temp.path() + "/pipeline.json");
        QFile exported(temp.path() + "/pipeline.json");
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(Json::parse(exported.readAll().toStdString()), report);
        const auto serial = view.requestId();
        view.setContext("compute", 36666);
        QVERIFY(!view.finish(serial, report));
        QVERIFY(view.finish(view.requestId(), compute));
        QVERIFY(!view.findChild<QPlainTextEdit *>()->toPlainText().contains("InverseSourceAlpha"));
    }
    void resourceHistoryShaderVisibility() {
        using Json = nlohmann::json;
        const auto root = qEnvironmentVariable("FLORA_DX12_USAGE_VISIBILITY_EVIDENCE");
        if (root.isEmpty())
            QSKIP("Set the BF1 shader-visibility resource history evidence directory");
        const auto read = [&](const char *name) {
            QFile file(root + '/' + name + "/result.json");
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Missing resource history evidence");
            return Json::parse(file.readAll().toStdString());
        };
        const auto oldInput = read("bf1-color-input-history-stock1");
        const auto input = read("bf1-color-input-history-fixed1");
        const auto oldOutput = read("bf1-premotion-history1");
        const auto output = read("bf1-premotion-history-fixed1");
        const auto count = [](const Json &report, const char *execution, const char *usage) {
            size_t found = 0;
            for (const auto &row : report.at("resource_history"))
                if (row.at("execution") == execution && row.at("usage_name") == usage)
                    ++found;
            return found;
        };
        QCOMPARE(count(oldInput, "36659", "PS_Resource"), size_t(0));
        QCOMPARE(oldInput.at("backend_dx12_usage_visibility_patch"), Json(0));
        QCOMPARE(oldInput.at("history_scope").at("shader_stage_binding_status"),
                 Json("known_stage_visibility_gap"));
        QCOMPARE(count(input, "36659", "PS_Resource"), size_t(1));
        QCOMPARE(count(oldOutput, "36758", "VS_Resource"), size_t(1));
        QCOMPARE(count(output, "36758", "VS_Resource"), size_t(0));
        QCOMPARE(count(output, "36758", "PS_Resource"), size_t(1));
        QCOMPARE(count(output, "36666", "CS_RWResource"), size_t(1));
        for (const auto *report : {&input, &output}) {
            QVERIFY(report->at("ok").get<bool>());
            QVERIFY(report->at("gpa_modules_loaded").empty());
            QCOMPARE(report->at("backend_dx12_usage_visibility_patch"), Json(1));
            QCOMPARE(report->at("history_scope").at("shader_stage_binding_status"),
                     Json("stage_visibility_matched"));
            QVERIFY(report->at("partial").get<bool>());
        }
        ResourceHistoryView view;
        view.setContext("bf1-stage-usage", 36659, 324843, 10432919056ULL);
        QVERIFY(view.finish(view.requestId(), input));
        auto rows = view.findChild<QTreeWidget *>("resourceHistoryRows");
        QTreeWidgetItem *selected = nullptr;
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->text(0) == "36659" &&
                rows->topLevelItem(i)->text(1) == "PS_Resource")
                selected = rows->topLevelItem(i);
        QVERIFY(selected);
        QCOMPARE(selected->text(2), QString("bound_use"));
        rows->setCurrentItem(selected);
        QSignalSpy jump(&view, &ResourceHistoryView::eventRequested);
        view.findChild<QAction *>("locateResourceHistoryEvent")->trigger();
        QCOMPARE(jump.count(), 1);
        QCOMPARE(jump[0][0].toULongLong(), 36659ULL);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        view.exportResult(temp.path() + "/history.json");
        QFile exported(temp.path() + "/history.json");
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(Json::parse(exported.readAll().toStdString()), input);
        QVERIFY(view.finish(view.requestId(), oldInput));
        auto summary = view.findChild<QLabel *>("resourceHistorySummary");
        QVERIFY(summary->text().contains("Overlapping stage registers"));
        QVERIFY(view.finish(view.requestId(), input));
        QVERIFY(!summary->text().contains("Overlapping stage registers"));
        const auto serial = view.requestId();
        view.setContext("other-frame", 36659, 324843, 10432919056ULL);
        QVERIFY(!view.finish(serial, input));
        QCOMPARE(rows->topLevelItemCount(), 0);
    }
    void packedFloatWorkspaceReference() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto project = qEnvironmentVariable("FLORA_DX12_PACKED_FLOAT_PROJECT");
        const auto output = qEnvironmentVariable("FLORA_DX12_PACKED_FLOAT_WORKSPACE");
        if (root.isEmpty() || project.isEmpty() || output.isEmpty())
            QSKIP("Set BF1 capture, workgroup experiment project and packed float workspace output");
        QVERIFY(QDir().mkpath(output));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window;
        window.resize(1500, 1000);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished);
        window.openCapture(root + "/bf1_2025_12_10__14_29_55.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.openDx12Experiment(project);
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000);
        QVERIFY(prepared.takeLast()[0].toBool());
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("36666");
        QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        QModelIndex selected;
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 299115) {
                selected = index;
                break;
            }
        }
        QVERIFY(selected.isValid());
        resources->setCurrentIndex(selected);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
        done.clear();
        window.replay();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QVERIFY(!window.busy());
        auto image = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(image->image().size(), QSize(1920, 1009));
        const auto color = QColor(255, 128, 64, 255);
        QImage expected(1920, 1009, QImage::Format_RGBA8888);
        expected.fill(color);
        QCOMPARE(image->image().convertToFormat(QImage::Format_RGBA8888), expected);
        QVERIFY(image->image().save(output + "/preview.png"));
        QSignalSpy exported(&window, &MainWindow::exportFinished);
        QTimer chooser, watchdog;
        watchdog.setSingleShot(true);
        bool chosen = false;
        connect(&chooser, &QTimer::timeout, &window, [&] {
            if (auto dialog = window.findChild<QFileDialog *>()) {
                chooser.stop();
                chosen = true;
                dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + "/storage.bin");
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        connect(&watchdog, &QTimer::timeout, &window, [&] {
            if (auto dialog = window.findChild<QFileDialog *>())
                dialog->reject();
        });
        chooser.start(10);
        watchdog.start(5000);
        window.findChild<QAction *>("exportOutputStorage")->trigger();
        QVERIFY(chosen);
        QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000);
        QVERIFY(exported.takeLast()[0].toBool());
        QFile raw(output + "/storage.bin");
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(raw.readAll(), QCryptographicHash::Sha256).toHex(),
                 QByteArray("97bc49991a49c2a76383d607da01363aeb595af412fc3fa8b6f9e6466de42b1f"));
        QFile metadata(output + "/storage.bin.json");
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto storage = nlohmann::json::parse(metadata.readAll().toStdString());
        QCOMPARE(storage.at("format"), nlohmann::json(26));
        QVERIFY(storage.at("display_interpretation").get<std::string>().find("linear_clamp_0_to_1") !=
                std::string::npos);
        auto cs = window.findChild<ReplayDebugView *>("replayDebug-cs");
        cs->findChild<QLineEdit *>("debugGroup")->setText("209 60 0");
        cs->findChild<QLineEdit *>("debugThread")->setText("2 5 0");
        image->pixelSelected(1674, 485, color);
        const auto request = cs->request();
        QCOMPARE(request.at("resource"), nlohmann::json("10432933744"));
        QCOMPARE(request.at("resource_creation"), nlohmann::json("299115"));
        QCOMPARE(request.at("x"), nlohmann::json(1674));
        QCOMPARE(request.at("y"), nlohmann::json(485));
        QCOMPARE(request.at("group"), nlohmann::json::array({209, 60, 0}));
        QCOMPARE(request.at("thread"), nlohmann::json::array({2, 5, 0}));
        QVERIFY(cs->findChild<QAction *>("replayDebugRead")->isEnabled());
        write(output + "/debug-request.json", QByteArray::fromStdString(request.dump(2)));
        const auto old = cs->requestId();
        locate->setText("36671");
        QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(!cs->request().contains("resource"));
        QVERIFY(!cs->finish(old, {{"ok", false}, {"error", "stale reference"}}));
    }
    void dx12ReadOnlyDepthEvidence() {
        using Json = nlohmann::json;
        const auto evidence = qEnvironmentVariable("FLORA_DX12_DEPTH_SNAPSHOT_EVIDENCE");
        if (evidence.isEmpty()) QSKIP("Set read-only depth GPU reproduction evidence directory");
        const auto read = [&](const QString &name) {
            QFile file(evidence + '/' + name);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing depth reproduction evidence");
            return file.readAll();
        };
        const auto color = QByteArray::fromHex("003c00380034003c");
        for (const auto &prefix : {QString("bf1-depth-snapshot"), QString("bf1-raster-depth")}) {
            const auto native = read(prefix + "-native1/output_storage.bin");
            const auto enabled = read(prefix + "-enabled1/texture.bin");
            const auto disabled = read(prefix + "-disabled1/texture.bin");
            QCOMPARE(native.size(), qsizetype(960 * 504 * 8));
            QCOMPARE(enabled, native);
            const auto report = Json::parse(read(prefix + "-enabled1/result.json").toStdString());
            QVERIFY(report.at("ok").get<bool>());
            QCOMPARE(report.at("execution"), Json("36560"));
            QCOMPARE(report.at("resource_creation"), Json("325012"));
            QCOMPARE(report.at("readonly_depth_snapshot").at("copies"), Json("1"));
            QVERIFY(report.at("gpa_modules_loaded").empty());
            const auto offReport = Json::parse(read(prefix + "-disabled1/result.json").toStdString());
            QVERIFY(!offReport.contains("readonly_depth_snapshot"));
            const auto even = (241 * 960 + 836) * 8;
            QCOMPARE(native.mid(even + 8, 8), color);
            if (prefix == "bf1-raster-depth") {
                QVERIFY(disabled != native);
                QCOMPARE(native.mid(even, 8), color);
                QCOMPARE(disabled.mid(even, 8), QByteArray(8, '\0'));
            } else {
                QCOMPARE(disabled, native);
                QCOMPARE(native.mid(even, 8), QByteArray(8, '\0'));
            }
        }
    }
    void dx12ReadOnlyDepthProject() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_DEPTH_SNAPSHOT_PROJECT");
        if (root.isEmpty() || output.isEmpty())
            QSKIP("Set BF1 capture and read-only depth reproduction project output");
        QVERIFY(QDir().mkpath(output));
        const auto capture = std::filesystem::path(
            (root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString());
        const auto session = openReplaySession(capture);
        dx12::StreamDirectory directory(capture);
        const auto &setter = directory.packets().at(255921 - 1);
        QCOMPARE(setter.type, uint64_t(0x1ae3c82b462fcULL));
        format::Reader reader(directory.payload(setter));
        reader.skip(8);
        const auto object = reader.read<uint64_t>();
        reader.end();
        uint64_t creation{};
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &packet = directory.packets()[i];
            if (!dx12::isPipelineRecipe(packet.type)) continue;
            const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
            if (recipe.object != object) continue;
            creation = i + 1;
            QCOMPARE(recipe.graphics.DepthStencilState.DepthEnable, TRUE);
            QCOMPARE(recipe.graphics.DepthStencilState.DepthWriteMask, D3D12_DEPTH_WRITE_MASK_ZERO);
            QCOMPARE(recipe.graphics.DepthStencilState.DepthFunc, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
            QCOMPARE(recipe.graphics.DepthStencilState.StencilEnable, FALSE);
        }
        QVERIFY(creation);
        // Exercise both sides of the real depth test with deterministic colors.
        // Odd columns always pass [0,1] depth. Even columns fail positive depth.
        const QByteArray source = R"(
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
Output main(float4 position : SV_Position) {
    Output result;
    result.color = float4(1.0, 0.5, 0.25, 1.0);
    result.depth = ((uint)position.x & 1) ? 1.0 : 0.0;
    return result;
})";
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        const auto hr = D3DCompile(source.data(), size_t(source.size()), "depth-snapshot-reference.hlsl",
                                  nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS,
                                  0, &code, &errors);
        QVERIFY2(SUCCEEDED(hr), errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Compile failed");
        write(output + "/depth-reference.hlsl", source);
        const format::Bytes bytes(static_cast<const uint8_t *>(code->GetBufferPointer()), code->GetBufferSize());
        Experiment experiment(*session);
        experiment.save(output + "/original.json");
        experiment.setShader(*session, creation, "PS", bytes);
        experiment.save(output + "/depth.json");
        Experiment loaded(*session);
        loaded.load(output + "/depth.json", *session);
        QCOMPARE(loaded.dx12Edits(*session).shaders.size(), size_t(1));
        QVERIFY(experiment.undo());
        QVERIFY(experiment.dx12Edits(*session).shaders.empty());
        experiment.save(output + "/restored.json");
        qInfo() << "Depth reproduction PS pipeline creation" << creation;
        const QByteArray fixedSource =
            "float4 main() : SV_Target0 { return float4(1.0, 0.5, 0.25, 1.0); }";
        code.Reset();
        errors.Reset();
        const auto fixedHr = D3DCompile(fixedSource.data(), size_t(fixedSource.size()),
                                       "raster-depth-reference.hlsl", nullptr, nullptr, "main", "ps_5_0",
                                       D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        QVERIFY2(SUCCEEDED(fixedHr), errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Compile failed");
        write(output + "/raster-depth-reference.hlsl", fixedSource);
        experiment.setShader(*session, creation, "PS",
                             format::Bytes(static_cast<const uint8_t *>(code->GetBufferPointer()), code->GetBufferSize()));
        experiment.save(output + "/raster-depth.json");
        QVERIFY(experiment.undo());
        QVERIFY(experiment.dx12Edits(*session).shaders.empty());
    }
    void dx12ComputeWorkgroupProject() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_WORKGROUP_PROJECT");
        if (root.isEmpty() || output.isEmpty())
            QSKIP("Set BF1 capture directory and workgroup reproduction project output");
        QVERIFY(QDir().mkpath(output));
        const auto session = openReplaySession(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        dx12::StreamDirectory directory(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        const auto &setter = directory.packets().at(213096 - 1);
        QCOMPARE(setter.type, uint64_t(0x1ae3c82b462fcULL));
        format::Reader reader(directory.payload(setter));
        reader.skip(8);
        const auto object = reader.read<uint64_t>();
        reader.end();
        uint64_t creation{};
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &packet = directory.packets()[i];
            if (dx12::isPipelineRecipe(packet.type) &&
                dx12::decodePipelineRecipe(packet.type, directory.payload(packet)).object == object)
                creation = i + 1;
        }
        QVERIFY(creation);
        // Use BF1's actual temporal CS output binding and group dimensions. The
        // neighbouring lane must carry this group's ID, not the default zero.
        const QByteArray source = R"(
RWTexture2D<float4> result : register(u3);
groupshared uint values[64];
[numthreads(8,8,1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex, uint3 pixel : SV_DispatchThreadID) {
    values[lane] = group.x + 256 * group.y + lane;
    GroupMemoryBarrierWithGroupSync();
    uint neighbour = (lane + 1) % 64;
    float correct = values[neighbour] == group.x + 256 * group.y + neighbour ? 1.0 : 0.0;
    result[pixel.xy] = float4(correct, 0.5, 0.25, 1.0);
})";
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        const auto hr =
            D3DCompile(source.data(), size_t(source.size()), "workgroup-reference.hlsl", nullptr, nullptr,
                       "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
        QVERIFY2(SUCCEEDED(hr),
                 errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Compile failed");
        write(output + "/workgroup-reference.hlsl", source);
        const format::Bytes bytes(static_cast<const uint8_t *>(code->GetBufferPointer()),
                                  code->GetBufferSize());
        Experiment experiment(*session);
        experiment.save(output + "/original.json");
        experiment.setShader(*session, creation, "CS", bytes);
        experiment.save(output + "/workgroup.json");
        Experiment loaded(*session);
        loaded.load(output + "/workgroup.json", *session);
        QCOMPARE(loaded.dx12Edits(*session).shaders.size(), size_t(1));
        QCOMPARE(loaded.dx12Edits(*session).shaders.front().bytecode.size(), bytes.size());
        QVERIFY(experiment.undo());
        QVERIFY(experiment.dx12Edits(*session).shaders.empty());
        experiment.save(output + "/restored.json");
        qInfo() << "Workgroup reproduction CS pipeline creation" << creation;
    }
    void dx12PipelineWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        const auto output = qEnvironmentVariable("FLORA_DX12_PIPELINE_WORKSPACE");
        if (root.isEmpty() || library.isEmpty() || output.isEmpty())
            QSKIP("Set Talos capture, RenderDoc and pipeline workspace output directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto view = window.findChild<Dx12PipelineView *>(); QVERIFY(view);
        auto pipelineTabs = window.findChild<QTabWidget *>("pipelineTabs");
        QVERIFY(pipelineTabs); QCOMPARE(pipelineTabs->currentWidget(), view);
        view->setBackendPath(library, false);
        auto readAction = view->findChild<QAction *>("readDx12Pipeline"); QVERIFY(readAction->isEnabled());
        done.clear(); readAction->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 90000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        const auto report = view->result();
        QCOMPARE(report.at("execution"), nlohmann::json("12675"));
        QCOMPARE(report.at("captured_call").at("source_record"), nlohmann::json("185550"));
        QVERIFY(report.at("gpa_modules_loaded").empty());
        view->exportResult(output + "/pipeline.json");
        const auto capture = QString::fromStdString(report.at("capture").get<std::string>());
        QVERIFY(QFile::copy(QFileInfo(capture).dir().filePath("report.json"), output + "/recapture.json"));
        auto rows = view->findChild<QTreeWidget *>("dx12PipelineRows");
        bool foundConstant = false;
        for (int i = 0; i < rows->topLevelItemCount(); ++i) {
            const auto &row = report.at("pipeline_rows")[i];
            if (row.at("section") != "VS" || row.at("resource").value("resource_creation", nlohmann::json()) != "150566") continue;
            rows->setCurrentItem(rows->topLevelItem(i));
            view->findChild<QAction *>("inspectPipelineResource")->trigger();
            auto properties = window.findChild<QTreeWidget *>("properties");
            QVERIFY(properties); QCOMPARE(properties->topLevelItem(0)->text(0), QString("DX12 resource creation"));
            bool foundId = false;
            for (int child = 0; child < properties->topLevelItem(0)->childCount(); ++child) {
                auto item = properties->topLevelItem(0)->child(child);
                if (item->text(0) == "id") { QCOMPARE(item->text(1), QString("150566")); foundId = true; }
            }
            QVERIFY(foundId);
            foundConstant = true; break;
        }
        QVERIFY(foundConstant);
        const auto request = view->requestId();
        locate->setText("12676"); QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(view->result().is_null()); QVERIFY(!view->finish(request, report));
        qInfo() << "Talos pipeline execution 12675: shader constants linked to capture creation 150566";
        done.clear(); window.openCapture(root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(!pipelineTabs->isTabVisible(pipelineTabs->indexOf(view)));
        QVERIFY(!readAction->isEnabled()); QVERIFY(!view->finish(request, report));
        auto image = window.findChild<ImageView *>("frameOutput")->image().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(image.size(), QSize(2560, 1440));
        const QByteArray pixels(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
        QCOMPARE(QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex(),
                 QByteArray("2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"));
        auto api = window.findChild<QTableView *>("apiLog"); api->setCurrentIndex(api->model()->index(2, 0));
        QVERIFY(window.findChild<QAction *>("inspectGeometry")->isEnabled());
    }
    void nativeTimelineQueues() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set Talos capture for the local SDK/device configuration");
        dx12::StreamDirectory directory(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::NativeDevice device(directory, true); dx12::NativeTimeline timeline(device.device(), 3);
        uint64_t submission = 9007199254740993ULL;
        for (const auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY}) {
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = type;
            QVERIFY(SUCCEEDED(device.device()->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))));
            QVERIFY(SUCCEEDED(device.device()->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))));
            QVERIFY(SUCCEEDED(device.device()->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))));
            QVERIFY(SUCCEEDED(device.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
            const auto entry = timeline.begin(list.Get(), {{"submission", std::to_string(submission)},
                {"queue", std::to_string(submission)}, {"command_list", "7"}, {"generation", "1"},
                {"list_ordinal", "0"}, {"first_execution", nullptr}, {"execution_count", "0"}});
            timeline.end(list.Get(), entry); QVERIFY(SUCCEEDED(list->Close()));
            timeline.beforeSubmit(submission, queue.Get());
            ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
            QVERIFY(SUCCEEDED(queue->Signal(fence.Get(), 1)));
            QTRY_VERIFY_WITH_TIMEOUT(fence->GetCompletedValue() >= 1, 5000);
            timeline.afterComplete(submission, queue.Get()); ++submission;
        }
        const auto result = timeline.result(); QCOMPARE(result.at("entries").size(), size_t(3));
        size_t measured{}, calibrated{};
        for (const auto &row : result.at("entries")) {
            if (row.at("status") == "measured") { ++measured; QVERIFY(row.at("duration_seconds").get<double>() >= 0); }
            if (row.at("clock_scope") == "calibrated_cpu_qpc") ++calibrated;
            QVERIFY(row.at("first_execution").is_null());
        }
        QVERIFY(measured >= 2); QVERIFY(calibrated >= 2);
        qInfo() << "Queue timestamp support" << measured << "calibrated queues" << calibrated;
        const auto info = device.information();
        const auto output = qEnvironmentVariable("FLORA_DX12_TIMELINE_PROBE");
        if (!output.isEmpty()) {
            QFile file(output); QVERIFY(file.open(QIODevice::WriteOnly));
            const auto bytes = nlohmann::json({{"timeline", result}, {"device", info}}).dump(2);
            QCOMPARE(file.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
        }
        QCOMPARE(info.at("validation_error_count"), nlohmann::json("0"));
    }
    void dx12InputMeshArtifacts() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto artifact = qEnvironmentVariable("FLORA_DX12_INPUT_MESH");
        if (root.isEmpty() || artifact.isEmpty()) QSKIP("Set Talos capture and input mesh artifact");
        const auto read = [&](const QString &file) { QFile f(artifact + '/' + file); if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing input mesh test artifact"); return f.readAll(); };
        const auto result = nlohmann::json::parse(read("result.json").toStdString());
        QCOMPARE(result.at("stage"), nlohmann::json("VSIn")); QCOMPARE(result.at("execution"), nlohmann::json("12675"));
        QCOMPARE(result.at("vertex_count"), nlohmann::json(8053)); QCOMPARE(result.at("index_count"), nlohmann::json(42006));
        QVERIFY(result.at("gpa_modules_loaded").empty());
        dx12::StreamDirectory directory(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::SubmissionIndex submissions(directory); dx12::AddressIndex addresses(directory);
        const auto draw = submissions.execution(12675);
        const auto &recording = submissions.recordings().at(*submissions.recordingForRecord(draw.record));
        D3D12_VERTEX_BUFFER_VIEW vb{}; D3D12_INDEX_BUFFER_VIEW ib{};
        for (const auto id : recording.records) {
            if (id > draw.record) break; const auto &p = directory.packets()[size_t(id - 1)];
            format::Reader r(directory.payload(p));
            if (p.type == 0x1ae3cc2293674ULL) {
                r.skip(8); const auto start = r.read<UINT>(), count = r.read<UINT>();
                QVERIFY(r.flag()); QCOMPARE(r.read<uint64_t>(), uint64_t(count));
                for (UINT i = 0; i < count; ++i) { const auto view = r.read<D3D12_VERTEX_BUFFER_VIEW>(); if (start + i == 0) vb = view; }
            } else if (p.type == 0x1ae3ccbc6c8c9ULL) { r.skip(8); QVERIFY(r.flag()); ib = r.read<D3D12_INDEX_BUFFER_VIEW>(); }
        }
        QCOMPARE(vb.StrideInBytes, 12U); QCOMPARE(vb.SizeInBytes, 96636U); QCOMPARE(ib.Format, DXGI_FORMAT_R16_UINT);
        const auto compareCaptured = [&](uint64_t address, uint64_t length, const QByteArray &actual) {
            const auto binding = addresses.resolve(address, draw.record, length); bool found{};
            for (size_t i = 0; i < directory.initializationCount(); ++i) {
                const auto &p = directory.packets()[i]; if (p.type != 0x1ae3cd301bf84ULL) continue;
                format::Reader r(directory.payload(p)); r.skip(8); const auto destination = r.read<uint64_t>(), offset = r.read<uint64_t>();
                r.skip(16); const auto count = r.read<uint64_t>();
                if (destination != binding.object || offset > binding.offset || binding.offset - offset > count || length > count - (binding.offset - offset)) continue;
                const auto plan = dx12::prepareInitializationCopy(directory, i + 1, 4294967296ULL);
                const auto source = plan.sourceOffset + binding.offset - offset;
                QVERIFY(source <= plan.sourceData.bytes.size() && length <= plan.sourceData.bytes.size() - source);
                QCOMPARE(actual, QByteArray(reinterpret_cast<const char *>(plan.sourceData.bytes.data() + source), qsizetype(length)));
                qInfo() << "Exact input bytes" << binding.creation << binding.object << "copy record" << i + 1 << "length" << length;
                found = true;
            }
            QVERIFY(found);
        };
        compareCaptured(vb.BufferLocation, vb.SizeInBytes, read("input_vertices.bin"));
        compareCaptured(ib.BufferLocation, 42006 * 2, read("input_indices.bin"));
        ReplayMeshView view; view.setContext("talos-input", 12675, true);
        auto stage = view.findChild<QComboBox *>("replayMeshStage"); stage->setCurrentIndex(stage->findData("VSIn"));
        QCOMPARE(view.request().at("attribute"), nlohmann::json(0));
        QVERIFY(view.finish(view.requestId(), result, artifact));
        const auto table = view.findChild<QTableView *>("replayMeshPositions"); QCOMPARE(table->model()->rowCount(), 8053);
        QCOMPARE(table->model()->index(0, 1).data().toDouble(), -10.244478225708008);
        QCOMPARE(table->model()->index(0, 2).data().toDouble(), 19.756996154785156);
        QCOMPARE(table->model()->index(0, 3).data().toDouble(), 61.500404357910156);
        QCOMPARE(table->model()->index(0, 4).data().toDouble(), 1.0);
        QTemporaryDir exports;
        view.exportResult(exports.filePath("input.vbin")); view.exportResult(exports.filePath("input.ibin"));
        QFile exported(exports.filePath("input.vbin")); QVERIFY(exported.open(QIODevice::ReadOnly)); QCOMPARE(exported.readAll(), read("input_vertices.bin"));
        QFile exportedIndices(exports.filePath("input.ibin")); QVERIFY(exportedIndices.open(QIODevice::ReadOnly)); QCOMPARE(exportedIndices.readAll(), read("input_indices.bin"));
        const auto old = view.requestId(); view.findChild<QLineEdit *>("replayMeshAttribute")->setText("1");
        QVERIFY(view.result().is_null()); QVERIFY(!view.finish(old, result, artifact));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), result, artifact));
    }
    void dx12BufferWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto project = qEnvironmentVariable("FLORA_DX12_BUFFER_PROJECT");
        const auto output = qEnvironmentVariable("FLORA_DX12_BUFFER_OUTPUT");
        if (root.isEmpty() || project.isEmpty() || output.isEmpty()) QSKIP("Set Talos captures, prepared buffer project and new output");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto capture = root + "/Talos_2026_10_18__00_55_08.gpa_frame";
        dx12::StreamDirectory directory(std::filesystem::path(capture.toStdWString()));
        QByteArray original(16, 0); std::array<bool, 16> covered{};
        // Source buffer of captured CopyBufferRegion record 188930. Restore
        // this exact byte range from recorded CPU patches before its submission.
        for (size_t i = 0; i < 188936; ++i) {
            const auto &p = directory.packets().at(i); if (p.type != 0x20000000000000ULL) continue;
            format::Reader r(directory.payload(p)); if (r.read<uint64_t>() != 109473824) continue;
            const auto data = directory.resourceData(r.read<uint64_t>());
            format::Reader descriptor(directory.resources().bytes(data.descriptorOffset, 64)); descriptor.skip(32);
            const auto offset = descriptor.read<uint64_t>();
            for (size_t n = 0; n < covered.size(); ++n) {
                const auto byte = 32058656ULL + n;
                if (byte >= offset && byte - offset < data.bytes.size()) { original[qsizetype(n)] = char(data.bytes[size_t(byte - offset)]); covered[n] = true; }
            }
        }
        QVERIFY(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }));
        write(output + "/captured-original.bin", original);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window; QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(capture); QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 150566) { resources->setCurrentIndex(index); break; }
        }
        QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 150566ULL);
        window.openDx12Experiment(project); QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeLast()[0].toBool());
        window.findChild<QLineEdit *>("bufferOffset")->setText("32058656");
        window.findChild<QLineEdit *>("bufferLength")->setText("16");
        auto boundary = window.findChild<QComboBox *>("bufferBoundary"); boundary->setCurrentIndex(1);
        auto table = window.findChild<QTableView *>("bufferTable");
        const auto model = static_cast<BufferModel *>(table->model());
        auto read = window.findChild<QAction *>("readBuffer");
        for (const auto modified : {true, false}) {
            if (!modified) {
                window.findChild<QAction *>("undoExperiment")->trigger();
                QVERIFY(model->bytes().isEmpty());
                boundary->setCurrentIndex(2);
            }
            done.clear(); read->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(model->offset(), 32058656ULL);
            const float row[4]{0, 0, 0, 0.25f}; const QByteArray changed(reinterpret_cast<const char *>(row), sizeof(row));
            QCOMPARE(model->bytes(), modified ? changed : original);
            const auto name = modified ? QString("modified") : QString("restored");
            QTimer chooser, watchdog; watchdog.setSingleShot(true); bool chosen{};
            connect(&chooser, &QTimer::timeout, &window, [&] {
                if (auto dialog = window.findChild<QFileDialog *>()) {
                    chosen = true; chooser.stop();
                    dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + '/' + name + ".bin");
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            connect(&watchdog, &QTimer::timeout, &window, [&] { if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject(); });
            chooser.start(10); watchdog.start(5000); window.findChild<QAction *>("exportBuffer")->trigger(); QVERIFY(chosen);
            QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
            QFile file(output + '/' + name + ".bin"); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), model->bytes());
            qInfo() << name << model->bytes().toHex();
        }
        locate->setText("12667"); QTest::keyClick(locate, Qt::Key_Return); QVERIFY(model->bytes().isEmpty());
    }
    void querySubmissionIdentityAndBounds() {
        QTemporaryDir temp; const auto path = fixture(temp);
        QByteArray packets; uint64_t timestamp{};
        const auto packet = [&](uint64_t type, const QByteArray &bytes) {
            qword(packets, type); qword(packets, ++timestamp); qword(packets, 0); qword(packets, bytes.size()); packets += bytes;
        };
        const uint64_t heap = UINT64_MAX - 4, queue = UINT64_MAX - 3, list = UINT64_MAX - 2;
        const auto resource = [&](uint64_t object, uint64_t width, D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_BUFFER) {
            QByteArray p; qword(p, 1); p.append(char(1));
            D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_READBACK;
            p.append(reinterpret_cast<const char *>(&properties), sizeof(properties)); dword(p, 0); p.append(char(1));
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = dimension; desc.Width = width;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            p.append(reinterpret_cast<const char *>(&desc), sizeof(desc)); dword(p, D3D12_RESOURCE_STATE_COPY_DEST); p.append(char(0));
            const auto iid = __uuidof(ID3D12Resource); p.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
            p.append(char(1)); qword(p, 1); qword(p, object); dword(p, 0); packet(0x12793f58d8f79ULL, p);
        };
        resource(42, 24); resource(43, 24, D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        QByteArray creation; qword(creation, 1); creation.append(char(1));
        dword(creation, 0); dword(creation, 3); dword(creation, 0);
        const auto iid = __uuidof(ID3D12QueryHeap); creation.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
        creation.append(char(1)); qword(creation, 1); qword(creation, heap); dword(creation, 0);
        packet(0x127937e9603edULL, creation);
        const auto reset = [&](uint64_t object) {
            QByteArray p; qword(p, object); qword(p, 17); qword(p, 0); dword(p, 0); packet(0x1ae3c721c351aULL, p);
        };
        const auto close = [&](uint64_t object) { QByteArray p; qword(p, object); dword(p, 0); packet(0x1ae3cd6acf667ULL, p); };
        const auto query = [&](uint64_t object, uint64_t type, uint32_t slot) {
            QByteArray p; qword(p, object); qword(p, heap); dword(p, 0); dword(p, slot); packet(type, p);
        };
        const auto resolve = [&](uint32_t count, bool truncate = false, uint64_t offset = 0, uint64_t destination = 42) {
            QByteArray p; qword(p, list); qword(p, heap); dword(p, 0); dword(p, 0); dword(p, count); qword(p, destination); qword(p, offset);
            if (truncate) p.chop(3); packet(0x1ae3cdc411c2dULL, p);
        };
        const auto submit = [&] {
            QByteArray p; qword(p, queue); dword(p, 1); p.append(char(1)); qword(p, 1); qword(p, list); packet(0x125482975df48ULL, p);
        };
        // Slot 2 exists in a recording that is never submitted.
        reset(9); query(9, 0x1ae3c6601dd2cULL, 2); query(9, 0x1ae3c64a8e138ULL, 2); close(9);
        reset(list); query(list, 0x1ae3c6601dd2cULL, 0); query(list, 0x1ae3c64a8e138ULL, 0); resolve(2); close(list);
        submit(); submit();
        reset(list); query(list, 0x1ae3c6601dd2cULL, 1); query(list, 0x1ae3c64a8e138ULL, 1); resolve(2); resolve(3); resolve(4); resolve(1, true); close(list); submit();
        reset(list); resolve(2, false, 16); resolve(2, false, 1); resolve(2, false, UINT64_MAX - 7);
        resolve(2, false, 0, 43); resolve(2, false, 0, 44); close(list); submit();
        // A reused captured pointer must use the new, smaller resource lifetime.
        resource(42, 8); const auto reusedCreation = timestamp;
        reset(list); resolve(2); close(list); submit();
        write(path + "/thread-7-0", packets);
        dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString())); dx12::SubmissionIndex submissions(directory);
        dx12::QueryReplayIndex queries(directory, submissions);
        QVERIFY(queries.firstIssue(3, true).is_null());
        QCOMPARE(queries.details(3).at("missing_count"), nlohmann::json(1));
        QCOMPARE(queries.details(3).at("source_record"), queries.details(6).at("source_record"));
        QVERIFY(queries.details(3).at("submission") != queries.details(6).at("submission"));
        QCOMPARE(queries.details(6).at("query_heap"), nlohmann::json(std::to_string(heap)));
        QCOMPARE(queries.details(9).at("status"), nlohmann::json("covered"));
        QCOMPARE(queries.details(10).at("missing_slots_with_recorded_end"), nlohmann::json(1));
        QCOMPARE(queries.details(10).at("missing_slots_without_recorded_end"), nlohmann::json(0));
        QCOMPARE(queries.details(11).at("status"), nlohmann::json("invalid_query_data"));
        QCOMPARE(queries.details(12).at("status"), nlohmann::json("invalid_query_data"));
        QCOMPARE(queries.details(9).at("result_bytes"), nlohmann::json("16"));
        QCOMPARE(queries.details(9).at("destination_resource").at("creation"), nlohmann::json("1"));
        QCOMPARE(queries.details(9).at("destination_resource").at("heap_type"), nlohmann::json(3));
        for (uint64_t execution = 13; execution <= 18; ++execution)
            QCOMPARE(queries.details(execution).at("status"), nlohmann::json("invalid_query_data"));
        QCOMPARE(queries.details(18).at("destination_resource").at("creation"), nlohmann::json(std::to_string(reusedCreation)));
        QCOMPARE(queries.details(18).at("destination_resource").at("width"), nlohmann::json("8"));
        const auto partial = dx12::queryResultRanges(queries.details(3));
        QCOMPARE(partial.size(), size_t(2)); QVERIFY(partial[0].available); QVERIFY(!partial[1].available);
        QCOMPARE(partial[0].offset, 0ULL); QCOMPARE(partial[1].offset, 8ULL); QCOMPARE(partial[1].bytes, 8ULL);
        QCOMPARE(dx12::queryResultRanges(queries.details(6))[1].offset, 8ULL);
        QCOMPARE(dx12::queryResultRanges(queries.details(9)).size(), size_t(1));
        auto high = queries.details(3); const uint64_t highOffset = (1ULL << 53) + 8;
        high["destination_offset"] = std::to_string(highOffset);
        high["destination_resource"]["width"] = std::to_string(highOffset + 16);
        QCOMPARE(dx12::queryResultRanges(high)[1].offset, highOffset + 8);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, queries.isolatedReadbacks(18, false));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, dx12::QueryReplayIndex(directory, submissions, [] { return true; }));
        const auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
        ReplaySelection selection; selection.execution = 3; selection.before = true;
        QVERIFY(session->capability(ReplayFeature::Replay, selection).supported());
        QCOMPARE(session->capability(ReplayFeature::Performance, selection).status, CapabilityStatus::MissingData);
        selection.before = false;
        QCOMPARE(session->capability(ReplayFeature::Replay, selection).status, CapabilityStatus::MissingData);
        QCOMPARE(session->executionDetails(3).at("query_replay_coverage"), queries.details(3));
    }
    void queryCompletionOrdering_data() {
        QTest::addColumn<QString>("mode"); QTest::addColumn<bool>("ordered");
        QTest::newRow("same queue") << QString("same") << true;
        QTest::newRow("CPU order only") << QString("unordered") << false;
        QTest::newRow("initial fence value") << QString("initial") << false;
        QTest::newRow("signal before query") << QString("early") << false;
        QTest::newRow("queue fence dependency") << QString("fence") << true;
        QTest::newRow("transitive fence dependency") << QString("transitive") << true;
        QTest::newRow("cyclic dependency") << QString("cycle") << false;
    }
    void queryCompletionOrdering() {
        QFETCH(QString, mode); QFETCH(bool, ordered);
        using Json = nlohmann::json;
        QTemporaryDir temp; const auto path = fixture(temp);
        QByteArray packets; uint64_t record{};
        const uint64_t heap = UINT64_MAX - 20, queue = UINT64_MAX - 30, fence = UINT64_MAX - 40;
        const auto packet = [&](uint64_t type, const QByteArray &bytes) {
            qword(packets, type); qword(packets, ++record); qword(packets, 0); qword(packets, bytes.size()); packets += bytes;
            return record;
        };
        QByteArray p; qword(p, 1); p.append(char(1));
        dword(p, D3D12_QUERY_HEAP_TYPE_TIMESTAMP); dword(p, 1); dword(p, 0);
        auto iid = __uuidof(ID3D12QueryHeap); p.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
        p.append(char(1)); qword(p, 1); qword(p, heap); dword(p, 0); packet(0x127937e9603edULL, p);
        p.clear(); qword(p, 1); p.append(char(1));
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_READBACK;
        p.append(reinterpret_cast<const char *>(&properties), sizeof(properties)); dword(p, 0); p.append(char(1));
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 8;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        p.append(reinterpret_cast<const char *>(&desc), sizeof(desc)); dword(p, D3D12_RESOURCE_STATE_COPY_DEST); p.append(char(0));
        iid = __uuidof(ID3D12Resource); p.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
        p.append(char(1)); qword(p, 1); qword(p, 42); dword(p, 0); packet(0x12793f58d8f79ULL, p);
        const auto createFence = [&](uint64_t object, uint64_t initial) {
            QByteArray b; qword(b, 1); qword(b, initial); dword(b, 0);
            const auto id = __uuidof(ID3D12Fence); b.append(reinterpret_cast<const char *>(&id), sizeof(id));
            b.append(char(1)); qword(b, 1); qword(b, object); dword(b, 0); packet(0x127930cf4f3a0ULL, b);
        };
        const auto sync = [&](bool wait, uint64_t q, uint64_t f) {
            QByteArray b; qword(b, q); qword(b, f); qword(b, 1); dword(b, 0);
            return packet(wait ? 0x12548db3bfde2ULL : 0x12548dab1bf2dULL, b);
        };
        for (uint64_t list : {10, 11}) {
            p.clear(); qword(p, list); qword(p, 17); qword(p, 0); dword(p, 0); packet(0x1ae3c721c351aULL, p);
            p.clear(); qword(p, list); qword(p, heap); dword(p, D3D12_QUERY_TYPE_TIMESTAMP); dword(p, 0);
            if (list == 11) { dword(p, 1); qword(p, 42); qword(p, 0); }
            packet(list == 10 ? 0x1ae3c64a8e138ULL : 0x1ae3cdc411c2dULL, p);
            p.clear(); qword(p, list); dword(p, 0); packet(0x1ae3cd6acf667ULL, p);
        }
        const auto submit = [&](uint64_t q, uint64_t list) {
            QByteArray b; qword(b, q); dword(b, 1); b.append(char(1)); qword(b, 1); qword(b, list);
            return packet(0x125482975df48ULL, b);
        };
        createFence(fence, mode == "initial" ? 1 : 0); createFence(fence + 1, 0);
        if (mode == "early") sync(false, queue, fence);
        if (mode == "cycle") sync(true, queue, fence + 1);
        const auto producer = submit(queue, 10);
        if (mode == "fence" || mode == "transitive" || mode == "cycle") sync(false, queue, fence);
        if (mode == "transitive") { sync(true, queue + 2, fence); sync(false, queue + 2, fence + 1); }
        if (mode != "same" && mode != "unordered") sync(true, queue + 1, mode == "transitive" ? fence + 1 : fence);
        const auto consumer = submit(mode == "same" ? queue : queue + 1, 11);
        if (mode == "cycle") sync(false, queue + 1, fence + 1);
        write(path + "/thread-7-0", packets);
        dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
        const dx12::QueueDependencyIndex dependencies(directory);
        QCOMPARE(dependencies.precedes(producer, consumer), ordered);
        QVERIFY(!dependencies.precedes(consumer, producer));
        QVERIFY(!dependencies.precedes(producer, producer));
        QVERIFY(!dependencies.precedes(0, consumer));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, dependencies.precedes(producer, consumer, [] { return true; }));
        const dx12::SubmissionIndex submissions(directory); const dx12::QueryReplayIndex queries(directory, submissions);
        const auto report = queries.details(2);
        QCOMPARE(report.at("status"), Json(ordered ? "covered" : "unresolved_query_dependency"));
        QCOMPARE(report.at("completion_order_verified"), Json(ordered));
        QCOMPARE(report.at("completed_count"), Json(1)); QCOMPARE(report.at("missing_count"), Json(0));
        QCOMPARE(report.at("unordered_completed_count"), Json(ordered ? 0 : 1));
        QCOMPARE(report.at("cross_queue_completed_count"), Json(mode == "same" ? 0 : 1));
        QVERIFY(queries.firstIssue(2, true).is_null());
        QCOMPARE(queries.firstIssue(2, false).is_null(), ordered);
        if (!ordered) {
            const auto &source = report.at("unordered_completed_examples")[0];
            QCOMPARE(source.at("producer_queue"), Json(std::to_string(queue)));
            QCOMPARE(source.at("producer_submission"), Json(std::to_string(producer)));
            QCOMPARE(source.at("end_execution"), Json("1"));
            const auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
            ReplaySelection selection; selection.execution = 2;
            const auto capability = session->capability(ReplayFeature::Replay, selection);
            QVERIFY(!capability.supported()); QVERIFY(capability.reason.find("Fence dependency") != std::string::npos);
        }
    }
    void queryResultLayouts() {
        QCOMPARE(dx12::queryResultStride(D3D12_QUERY_TYPE_OCCLUSION), 8U);
        QCOMPARE(dx12::queryResultStride(D3D12_QUERY_TYPE_PIPELINE_STATISTICS), 88U);
        QCOMPARE(dx12::queryResultStride(D3D12_QUERY_TYPE_PIPELINE_STATISTICS1), 112U);
        QCOMPARE(dx12::queryResultStride(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3), 16U);
        dx12::validateQueryDestination(D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 2, 8, 232);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::validateQueryDestination(D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 2, 8, 231));
        const uint64_t offset = 1ULL << 33;
        dx12::validateQueryDestination(D3D12_QUERY_TYPE_TIMESTAMP, 2, offset, offset + 16);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::validateQueryDestination(D3D12_QUERY_TYPE_TIMESTAMP, 2, UINT64_MAX - 7, UINT64_MAX));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::queryResultStride(UINT32_MAX));
    }
    void queryReadbackAvailability() {
        // Overwriting part of a prior result with a missing query must revoke
        // those bytes, even if an earlier execution populated the allocation.
        dx12::QueryReadbackState state(UINT64_MAX);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, state.requireReadable(0, 8));
        state.write(8, 32, true); state.write(16, 8, false);
        state.requireReadable(8, 8); state.requireReadable(24, 16);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, state.requireReadable(8, 17));
        state.write(16, 8, true); state.requireReadable(8, 32);
        state.write(0, 16, false); state.requireReadable(16, 24);
        state.write(32, 32, false); state.requireReadable(16, 16);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, state.requireReadable(16, 17));
        const auto high = (1ULL << 53) + 8;
        state.write(high, 16, true); state.requireReadable(high, 16);
        state.write(UINT64_MAX - 8, 8, true); state.requireReadable(UINT64_MAX - 8, 8);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, state.write(UINT64_MAX - 8, 9, true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, state.requireReadable(UINT64_MAX - 8, 9));
        const auto report = state.information();
        QCOMPARE(report.at("width"), nlohmann::json("18446744073709551615"));
        QVERIFY(report.dump().find(std::to_string(high)) != std::string::npos);
        // Exercise interval splits/merges against an independent byte mask.
        dx12::QueryReadbackState maskState(32); std::array<bool, 32> expected{};
        for (uint32_t i = 0; i < 96; ++i) {
            const uint32_t offset = (i * 7) % 32, count = 1 + (i * 11) % (32 - offset);
            const bool available = i % 3 != 0;
            maskState.write(offset, count, available);
            std::fill(expected.begin() + offset, expected.begin() + offset + count, available);
            for (uint32_t at = 0; at < 32; ++at) for (uint32_t n = 1; n <= 32 - at; ++n) {
                bool readable = true;
                try { maskState.requireReadable(at, n); } catch (const std::runtime_error &) { readable = false; }
                QCOMPARE(readable, std::all_of(expected.begin() + at, expected.begin() + at + n, [](bool b) { return b; }));
            }
        }
    }
    void talosQueryDestinations() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_QUERY_DESTINATIONS_OUTPUT");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set Talos capture and a new query destination evidence file");
        dx12::StreamDirectory directory(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::SubmissionIndex submissions(directory); dx12::QueryReplayIndex queries(directory, submissions);
        const auto first = queries.details(16001), second = queries.details(19409);
        QCOMPARE(first.at("missing_count"), nlohmann::json(140));
        QCOMPARE(second.at("missing_count"), nlohmann::json(146));
        for (const auto &entry : {first, second}) {
            QCOMPARE(entry.at("status"), nlohmann::json("missing_query_results"));
            QCOMPARE(entry.at("result_bytes"), nlohmann::json("1888"));
            const auto &resource = entry.at("destination_resource");
            QCOMPARE(resource.at("heap_type"), nlohmann::json(3));
            QCOMPARE(resource.at("heap_creation"), nlohmann::json("42957"));
            QCOMPARE(resource.at("width"), nlohmann::json("65536"));
        }
        QCOMPARE(first.at("destination_resource").at("creation"), nlohmann::json("78132"));
        QCOMPARE(second.at("destination_resource").at("creation"), nlohmann::json("12800"));
        QCOMPARE(first.at("destination_resource").at("heap_offset"), nlohmann::json("327680"));
        QCOMPARE(second.at("destination_resource").at("heap_offset"), nlohmann::json("458752"));
        QVERIFY(!QFileInfo::exists(output)); write(output, QByteArray::fromStdString(queries.information().dump(2)));
    }
    void talosIsolatedQueryReplay() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_ISOLATED_QUERY_OUTPUT");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set Talos capture directory and a new isolated-query GPU evidence directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        dx12::StreamDirectory directory(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::SubmissionIndex submissions(directory); dx12::QueryReplayIndex queries(directory, submissions);
        QCOMPARE(queries.isolatedReadbacks(16001, true), (std::set<uint64_t>{12800, 78132}));
        dx12::NativeDevice device(directory, true);
        {
            dx12::NativeResources resources(device, directory, 4ULL * 1024 * 1024 * 1024);
            for (const auto creation : queries.isolatedReadbacks(19410, false)) resources.isolateQueryReadback(creation);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resources.resource(1643440736, 188923));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resources.gpuAddress(12696485888ULL, 188923, 8));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resources.tileHeap(1646217152, 188923));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resources.readableBuffer(1643440736, 188923, 0, 8));
            QCOMPARE(resources.information().at("created_resources"), nlohmann::json(0));
        }
        // An alias introduced in any later captured placement must be rejected
        // before allocation, even if the selected replay prefix does not use it.
        {
            QTemporaryDir temp; const auto path = fixture(temp); QByteArray packets; uint64_t time{};
            for (const auto id : {42957ULL, 78132ULL, 12800ULL}) {
                const auto &original = directory.packets()[id - 1]; const auto bytes = directory.payload(original);
                QByteArray body(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
                if (id == 12800) { const uint64_t aliasOffset = 327680; memcpy(body.data() + 16, &aliasOffset, 8); }
                qword(packets, original.type); qword(packets, ++time); qword(packets, 0); qword(packets, body.size()); packets += body;
            }
            write(path + "/thread-7-0", packets);
            dx12::StreamDirectory aliases(std::filesystem::path(path.toStdWString()));
            dx12::NativeResources resources(device, aliases, 4ULL * 1024 * 1024 * 1024);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, resources.isolateQueryReadback(2));
            QCOMPARE(resources.information().at("created_resources"), nlohmann::json(0));
        }
        dx12::NativeReplay replay(device, directory, 4ULL * 1024 * 1024 * 1024);
        replay.setIsolateMissingQueries(true); replay.prepareExecution(19410, true); replay.initializeTransfers();
        replay.replayTo(19410, true);
        const auto texture = replay.readTexture(1651480816, 0,
            std::filesystem::path((output + "/output_storage.bin").toStdWString()), 35118);
        auto reads = nlohmann::json::array();
        for (const auto execution : {16001ULL, 19409ULL}) {
            const auto entry = queries.details(execution);
            const auto creation = std::stoull(entry.at("destination_resource").at("creation").get<std::string>());
            const auto ranges = dx12::queryResultRanges(entry);
            bool inspected = false, rejected = false;
            for (const auto &range : ranges) {
                if (range.available && !inspected) {
                    reads.push_back(replay.inspectBuffer(creation, range.offset, range.bytes)); inspected = true;
                } else if (!range.available && !rejected) {
                    const auto filename = output + "/unavailable-" + QString::number(creation) + ".bin";
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.inspectBuffer(creation, range.offset, 1,
                        std::filesystem::path(filename.toStdWString())));
                    QVERIFY(!QFileInfo::exists(filename)); rejected = true;
                }
            }
            QVERIFY(inspected && rejected);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.inspectBuffer(creation, 0, 1888));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.inspectBuffer(creation, 1888, 8));
        }
        const nlohmann::json report{{"texture", texture}, {"known_query_reads", reads}, {"native_replay", replay.information()},
            {"device", device.information()}, {"unknown_reads_rejected", true}, {"ordinary_consumers_rejected", true},
            {"overlapping_placement_rejected", true}, {"frame_complete", false}};
        write(output + "/report.json", QByteArray::fromStdString(report.dump(2)));
        QCOMPARE(report.at("device").at("validation_error_count"), nlohmann::json("0"));
        QCOMPARE(texture.at("sha256"), nlohmann::json("e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb"));
    }
    void queryWorkspaceBoundary() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set captures for Talos query boundary navigation");
        MainWindow window; QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("16001"); QTest::keyClick(locate, Qt::Key_Return);
        auto boundary = window.findChild<QComboBox *>("outputBoundary");
        auto replay = window.findChild<QAction *>("replayFrame");
        boundary->setCurrentIndex(2); QVERIFY(!replay->isEnabled());
        QVERIFY(replay->toolTip().contains("140 of 236"));
        boundary->setCurrentIndex(1); QVERIFY(replay->isEnabled());
        boundary->setCurrentIndex(2); QVERIFY(!replay->isEnabled());
        locate->setText("16000"); QTest::keyClick(locate, Qt::Key_Return); QVERIFY(replay->isEnabled());
        QVERIFY(done.empty()); QVERIFY(!window.busy()); // Navigation does not launch GPU work.
        auto isolation = window.findChild<QCheckBox *>("dx12QueryIsolation"); QVERIFY(isolation);
        locate->setText("19410"); QTest::keyClick(locate, Qt::Key_Return); QVERIFY(!replay->isEnabled());
        isolation->setChecked(true); QVERIFY(replay->isEnabled());
        QVERIFY(window.findChild<QLabel *>("frameOutputLabel")->text().contains("Incomplete"));
        QVERIFY(window.findChild<QAction *>("openExperiment")->isEnabled());
        QVERIFY(window.findChild<QAction *>("replaceDx12Shader")->isEnabled());
        isolation->setChecked(false); QVERIFY(!replay->isEnabled());
        QVERIFY(done.empty());
        const auto session = openReplaySession(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        ReplaySelection selection; selection.execution = 19410; selection.isolateMissingQueries = true;
        QVERIFY(session->capability(ReplayFeature::Replay, selection).supported());
        QCOMPARE(session->capability(ReplayFeature::PixelHistory, selection).status, CapabilityStatus::MissingData);
        for (const auto feature : {ReplayFeature::ShaderDebug,
                                   ReplayFeature::Geometry, ReplayFeature::PipelineInspection, ReplayFeature::Performance, ReplayFeature::Experiments})
            QCOMPARE(session->capability(feature, selection).status, CapabilityStatus::NotImplemented);
        selection.resource = 78132;
        QVERIFY(session->capability(ReplayFeature::ResourceHistory,selection).supported());
        QCOMPARE(session->capability(ReplayFeature::Replay, selection).status, CapabilityStatus::NotImplemented);
        selection.execution = 19408;
        QVERIFY(session->capability(ReplayFeature::PipelineInspection,selection).supported());
        QVERIFY(session->capability(ReplayFeature::ShaderDebug,selection).supported());
        QVERIFY(session->capability(ReplayFeature::Geometry,selection).supported());
        QVERIFY(session->capability(ReplayFeature::Performance,selection).supported());
        QVERIFY(session->capability(ReplayFeature::Experiments,selection).supported());
        QVERIFY(!session->capability(ReplayFeature::PixelHistory,selection).supported()); // Query buffer is not a texture.
        selection.resource = 35118;
        QVERIFY(session->capability(ReplayFeature::PixelHistory,selection).supported());
        selection.isolateMissingQueries = false;
        QVERIFY(!session->capability(ReplayFeature::PipelineInspection,selection).supported());
        QVERIFY(!session->capability(ReplayFeature::ShaderDebug,selection).supported());
        QVERIFY(!session->capability(ReplayFeature::Geometry,selection).supported());
        QVERIFY(!session->capability(ReplayFeature::Performance,selection).supported());
        QVERIFY(!session->capability(ReplayFeature::PixelHistory,selection).supported());
    }
    void isolatedQueryOverlayWorkspace_data() {
        QTest::addColumn<QString>("overlay");
        QTest::newRow("shader") << QString("shader");
        QTest::newRow("buffer") << QString("buffer");
        QTest::newRow("texture") << QString("texture");
    }
    void isolatedQueryOverlayWorkspace() {
        QFETCH(QString, overlay);
        const bool shader = overlay == "shader", texture = overlay == "texture";
        using Json = nlohmann::json;
        try {
            const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
            const auto evidence = qEnvironmentVariable("FLORA_QUERY_OVERLAY_UI");
            const auto library = qEnvironmentVariable("FLORA_QUERY_HISTORY_RENDERDOC");
            if (captures.isEmpty() || evidence.isEmpty() || library.isEmpty())
                QSKIP("Set Talos captures, a new overlay output directory and the pinned RenderDoc library");
            const auto output = evidence + '/' + overlay;
            QVERIFY(!QFileInfo::exists(output));
            QVERIFY(QDir().mkpath(output));
            QByteArray replacement;
            if (shader) {
                const QByteArray source = "float4 main() : SV_Target { return float4(1,0,1,1); }";
                Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
                const auto hr =
                    D3DCompile(source.data(), size_t(source.size()), "isolated-replacement.hlsl", nullptr,
                               nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
                QVERIFY2(SUCCEEDED(hr),
                         errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Compile failed");
                replacement = QByteArray(static_cast<const char *>(blob->GetBufferPointer()),
                                         qsizetype(blob->GetBufferSize()));
                write(output + "/replacement.hlsl", source);
            } else if (texture) {
                const auto session = openReplaySession(std::filesystem::path(
                    (captures + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
                const auto description = Json::parse(session->resource(51141).summary);
                write(output + "/texture-description.json", QByteArray::fromStdString(description.dump(2)));
                QCOMPARE(description.at("Dimension"), Json(3));
                QCOMPARE(description.at("Width"), Json("1024"));
                QCOMPARE(description.at("Height"), Json(1024));
                QCOMPARE(description.at("MipLevels"), Json(1));
                QCOMPARE(description.at("Format"), Json(90)); // B8G8R8A8_TYPELESS storage.
                // Distinct storage channels verify BGRA preview ordering. Zero
                // alpha keeps the selected Draw transparent without changing PS.
                replacement = QByteArray::fromHex("21436500").repeated(1024 * 1024);
            } else {
                // PS cb0[0].zw are the alpha scale/bias. Zero both before
                // submission 192532; keep all other bytes and the shader intact.
                replacement = QByteArray(8, '\0');
            }
            const auto asset = output + (shader    ? "/replacement.dxbc"
                                         : texture ? "/texture.bin"
                                                   : "/alpha.bin");
            write(asset, replacement);
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            MainWindow window;
            QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
            QSignalSpy prepared(&window, &MainWindow::experimentFinished),
                exported(&window, &MainWindow::exportFinished);
            window.openCapture(captures + "/Talos_2026_10_18__00_55_08.gpa_frame");
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
            window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
            auto locate = window.findChild<QLineEdit *>("apiLocate");
            locate->setText("19408");
            QTest::keyClick(locate, Qt::Key_Return);
            auto resources = window.findChild<QTableView *>("resources");
            for (int row = 0; row < resources->model()->rowCount(); ++row) {
                const auto index = resources->model()->index(row, 0);
                if (index.data(Qt::UserRole).toULongLong() == 35118) {
                    resources->setCurrentIndex(index);
                    break;
                }
            }
            QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 35118ULL);
            window.findChild<QCheckBox *>("dx12QueryIsolation")->setChecked(true);
            window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
            const auto exportRaw = [&](const QString &name) {
                QTimer chooser, watchdog;
                watchdog.setSingleShot(true);
                bool chosen = false;
                connect(&chooser, &QTimer::timeout, &window, [&] {
                    if (auto dialog = window.findChild<QFileDialog *>()) {
                        chooser.stop();
                        chosen = true;
                        dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + '/' + name + ".bin");
                        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    }
                });
                connect(&watchdog, &QTimer::timeout, &window, [&] {
                    if (auto dialog = window.findChild<QFileDialog *>())
                        dialog->reject();
                });
                chooser.start(10);
                watchdog.start(5000);
                window.findChild<QAction *>("exportOutputStorage")->trigger();
                QVERIFY(chosen);
                QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000);
                QVERIFY(exported.takeLast()[0].toBool());
            };
            const auto read = [&](const QString &name) {
                QFile file(output + '/' + name);
                if (!file.open(QIODevice::ReadOnly))
                    throw std::runtime_error("Missing overlay evidence");
                return file.readAll();
            };
            if (shader)
                window.replaceDx12Shader(156511, "PS", asset);
            else if (texture)
                window.patchDx12Resource(192532, 51141, 0, 0, true, asset);
            else
                window.patchDx12Resource(192532, 150566, 32866312, 0, false, asset);
            QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000);
            QVERIFY(prepared.takeLast()[0].toBool());
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            QVERIFY(window.findChild<QCheckBox *>("dx12QueryIsolation")->isEnabled());
            exportRaw("modified");
            QVERIFY(window.saveDx12Experiment(output + "/modified.json"));
            const auto modified = read("modified.bin");
            QCOMPARE(modified.size(), 8294400);
            QCOMPARE(modified.mid((172 * 1920 + 65) * 4, 4).toHex(),
                     shader ? QByteArray("ff00ffff") : QByteArray("020606d9"));
            if (!shader && !texture)
                QCOMPARE(QCryptographicHash::hash(modified, QCryptographicHash::Sha256).toHex(),
                         QByteArray("5e3aae068d6743e52729350ec8e958861a21e5c800bad41b33946dddc4450f82"));
            // Texture import affects every use after the submission boundary;
            // unlike this per-draw constant range, it need not match Draw disable.
            const auto metadata = Json::parse(read("modified.bin.json").toStdString());
            QCOMPARE(metadata.at("frame_complete"), Json(false));
            QCOMPARE(metadata.at("missing_query_policy"), Json("isolate_readback_outputs"));
            auto debug = window.findChild<ReplayDebugView *>("replayDebug-ps");
            debug->setBackendPath(library, false);
            debug->selectPixel(65, 172, 0);
            debug->selectPrimitive(0);
            auto debugRead = debug->findChild<QAction *>("replayDebugRead");
            QVERIFY(debugRead->isEnabled());
            done.clear();
            debugRead->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 180000);
            QVERIFY2(done.takeLast()[0].toBool(),
                     qPrintable(debug->findChild<QLabel *>("replayDebugSummary")->text()));
            QVERIFY(debug->model());
            const auto result = debug->model()->result();
            write(output + "/debug.json", QByteArray::fromStdString(result.dump(2)));
            const auto capture = QString::fromStdString(result.at("capture").get<std::string>());
            QVERIFY(
                QFile::copy(QFileInfo(capture).dir().filePath("report.json"), output + "/recapture.json"));
            const auto expectedShader =
                shader
                    ? QCryptographicHash::hash(replacement, QCryptographicHash::Sha256).toHex().toStdString()
                    : std::string("9ac2a66820c99072d1ee0bd792e6164206669e67a55fa30461d0a32cb3b42c2f");
            QCOMPARE(result.at("shader_bytecode").at("sha256"), Json(expectedShader));
            QCOMPARE(result.at("frame_complete"), Json(false));
            QCOMPARE(result.at("partial"), Json(true));
            QCOMPARE(result.at("execution"), Json("19408"));
            debug->findChild<QSpinBox *>("debugStepIndex")->setValue(int(debug->model()->size() - 1));
            debug->findChild<QLineEdit *>("debugWatchExpression")->setText("SV_Target.w");
            debug->findChild<QAction *>("debugAddWatch")->trigger();
            const auto watches = debug->model()->watchResults();
            QCOMPARE(watches.at(0).at("status"), Json("available"));
            QCOMPARE(watches.at(0).at("values"), Json::array({shader ? 1.0 : 0.0}));
            debug->exportResult(output + "/debug-export.json");
            // Inspect the same recapture, proving the bound stage is the active
            // replacement rather than the original cached shader.
            auto pipeline = window.findChild<Dx12PipelineView *>();
            pipeline->setBackendPath(library, false);
            done.clear();
            pipeline->findChild<QAction *>("readDx12Pipeline")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            pipeline->exportResult(output + "/pipeline.json");
            QCOMPARE(pipeline->result().at("capture"), result.at("capture"));
            bool found = false;
            const auto rows = pipeline->findChild<QTreeWidget *>("dx12PipelineRows");
            for (int row = 0; row < rows->topLevelItemCount(); ++row) {
                auto item = rows->topLevelItem(row);
                if (item->data(0, Qt::UserRole + 3).toString() != "PS")
                    continue;
                rows->setCurrentItem(item);
                found = true;
                const auto &entry =
                    pipeline->result().at("pipeline_rows").at(item->data(0, Qt::UserRole + 1).toULongLong());
                QCOMPARE(entry.at("details").at("bytecode_sha256"), Json(expectedShader));
                QVERIFY(pipeline->findChild<QAction *>("replaceBoundPipelineShader")->isEnabled());
            }
            QVERIFY(found);
            QString pixelContext;
            auto history = window.findChild<PixelHistoryView *>();
            uint64_t oldHistory{};
            Json pixelHistory;
            if (texture) {
                // Exercise the navigation slot with the current analysis context.
                // This does not claim that the PS has a compute texture-store row.
                const auto suffix =
                    ':' + QFileInfo(library).canonicalFilePath() + ':' +
                    QString::fromLatin1(QCryptographicHash::hash(
                                            [&] {
                                                QFile file(library);
                                                if (!file.open(QIODevice::ReadOnly))
                                                    throw std::runtime_error("Cannot read analysis library");
                                                return file.readAll();
                                            }(),
                                            QCryptographicHash::Sha256)
                                            .toHex());
                pixelContext = QString::fromStdString(result.at("experiment_key").get<std::string>());
                QVERIFY(pixelContext.endsWith(suffix));
                pixelContext.chop(suffix.size());
                QVERIFY(pixelContext.endsWith(":query-isolated"));
                auto wrongMode = pixelContext;
                wrongMode.chop(QString(":query-isolated").size());
                wrongMode += ":strict";
                auto image = window.findChild<ImageView *>("frameOutput");
                const auto beforeJump = image->image();
                debug->texturePixelRequested(wrongMode, 19408, 51141, 4146950352ULL, 0, 0, 0, 0, 0);
                QCOMPARE(image->image(), beforeJump);
                debug->texturePixelRequested({}, 19408, 51141, 4146950352ULL, 0, 0, 0, 0, 0);
                QCOMPARE(image->image(), beforeJump);
                debug->texturePixelRequested(pixelContext, 19408, 51141, 4146950352ULL, 0, 0, 0, 0, 0);
                QVERIFY(image->image().isNull());
                QCOMPARE(history->request().at("resource_creation"), Json("51141"));
                done.clear();
                window.replay();
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
                QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                QCOMPARE(image->image().size(), QSize(1024, 1024));
                QCOMPARE(image->image().pixelColor(0, 0), QColor(101, 67, 33, 0));
                QCOMPARE(image->selectedPixel(), std::optional<QPoint>(QPoint(0, 0)));
                exportRaw("input-readback");
                QCOMPARE(read("input-readback.bin"), replacement);
                const auto input = Json::parse(read("input-readback.bin.json").toStdString());
                QCOMPARE(input.at("creation_record"), Json("51141"));
                QCOMPARE(input.at("format"), Json(90));
                QCOMPARE(input.at("display_interpretation"),
                         Json("typeless_bgra8_storage_as_unorm_no_color_space_transform"));
                QCOMPARE(input.at("missing_query_policy"), Json("isolate_readback_outputs"));
                debug->texturePixelRequested(pixelContext, 19408, 35118, 1651480816ULL, 65, 172, 0, 0, 0);
                QVERIFY(image->image().isNull());
                history->setBackendPath(library, false);
                done.clear();
                history->queryPixel(19408, 1651480816ULL, 65, 172, 0, 0, 0);
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
                QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                history->exportResult(output + "/history.json");
                pixelHistory = Json::parse(read("history.json").toStdString());
                QCOMPARE(pixelHistory.at("capture"), result.at("capture"));
                QCOMPARE(pixelHistory.at("frame_complete"), Json(false));
                QCOMPARE(pixelHistory.at("history").size(), size_t(3));
                const auto &last = pixelHistory.at("history").back();
                QCOMPARE(last.at("execution"), Json("19408"));
                QCOMPARE(last.at("source_record"), Json("192508"));
                for (size_t component = 0; component < 4; ++component) {
                    const auto expected = std::array<int, 4>{6, 6, 2, 217}[component];
                    QCOMPARE(
                        int(last.at("postMod").at("col").at("floatValue")[component].get<double>() * 255.0 +
                            0.5),
                        expected);
                }
                oldHistory = history->requestId();
            }
            const auto oldDebug = debug->requestId(), oldPipeline = pipeline->requestId();
            done.clear();
            window.findChild<QAction *>("undoExperiment")->trigger();
            QVERIFY(!debug->model());
            QVERIFY(pipeline->result().is_null());
            QVERIFY(!debug->finish(oldDebug, result));
            QVERIFY(!pipeline->finish(oldPipeline, {}));
            if (texture)
                QVERIFY(!history->finish(oldHistory, pixelHistory));
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            exportRaw("restored");
            QVERIFY(window.saveDx12Experiment(output + "/restored.json"));
            QCOMPARE(QCryptographicHash::hash(read("restored.bin"), QCryptographicHash::Sha256).toHex(),
                     QByteArray("e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb"));
            const auto restored = Json::parse(read("restored.bin.json").toStdString());
            QCOMPARE(restored.at("experiment").at("cursor"), Json("0"));
            QVERIFY(restored.at("experiment").at("document_sha256") !=
                    metadata.at("experiment").at("document_sha256"));
            if (texture) {
                auto image = window.findChild<ImageView *>("frameOutput");
                const auto restoredImage = image->image();
                debug->texturePixelRequested(pixelContext, 19408, 51141, 4146950352ULL, 0, 0, 0, 0, 0);
                QCOMPARE(image->image(), restoredImage);
                QVERIFY(!image->selectedPixel());
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void isolatedQueryExperimentWorkspace() {
        using Json = nlohmann::json;
        try {
            const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
            const auto output = qEnvironmentVariable("FLORA_QUERY_EXPERIMENT_UI");
            if (root.isEmpty() || output.isEmpty())
                QSKIP("Set Talos captures and a new experiment evidence directory");
            QVERIFY(!QFileInfo::exists(output));
            QVERIFY(QDir().mkpath(output));
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            MainWindow window;
            QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
            QSignalSpy prepared(&window, &MainWindow::experimentFinished),
                exported(&window, &MainWindow::exportFinished);
            window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
            window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
            auto locate = window.findChild<QLineEdit *>("apiLocate");
            locate->setText("19408");
            QTest::keyClick(locate, Qt::Key_Return);
            auto resources = window.findChild<QTableView *>("resources");
            for (int row = 0; row < resources->model()->rowCount(); ++row) {
                const auto index = resources->model()->index(row, 0);
                if (index.data(Qt::UserRole).toULongLong() == 35118) {
                    resources->setCurrentIndex(index);
                    break;
                }
            }
            QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 35118ULL);
            auto isolation = window.findChild<QCheckBox *>("dx12QueryIsolation");
            isolation->setChecked(true);
            auto boundary = window.findChild<QComboBox *>("outputBoundary");
            boundary->setCurrentIndex(2);
            auto enable = window.findChild<QAction *>("enableExperimentEvent");
            auto undo = window.findChild<QAction *>("undoExperiment");
            auto redo = window.findChild<QAction *>("redoExperiment");
            auto image = window.findChild<ImageView *>("frameOutput");
            auto debug = window.findChild<ReplayDebugView *>("replayDebug-vs");
            auto mesh = window.findChild<ReplayMeshView *>("replayMeshView");
            auto history = window.findChild<PixelHistoryView *>();
            auto metrics = window.findChild<RdcCountersView *>();
            const auto exportRaw = [&](const QString &name) {
                QTimer chooser, watchdog;
                watchdog.setSingleShot(true);
                bool chosen = false;
                connect(&chooser, &QTimer::timeout, &window, [&] {
                    if (auto dialog = window.findChild<QFileDialog *>()) {
                        chooser.stop();
                        chosen = true;
                        dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + '/' + name + ".bin");
                        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    }
                });
                connect(&watchdog, &QTimer::timeout, &window, [&] {
                    if (auto dialog = window.findChild<QFileDialog *>())
                        dialog->reject();
                });
                chooser.start(10);
                watchdog.start(5000);
                window.findChild<QAction *>("exportOutputStorage")->trigger();
                QVERIFY(chosen);
                QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000);
                QVERIFY(exported.takeLast()[0].toBool());
            };
            const auto read = [&](const QString &name) {
                QFile file(output + '/' + name);
                if (!file.open(QIODevice::ReadOnly))
                    throw std::runtime_error("Cannot read experiment evidence");
                return file.readAll();
            };
            window.replay();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            exportRaw("baseline");
            const auto baseline = read("baseline.bin");
            QCOMPARE(QCryptographicHash::hash(baseline, QCryptographicHash::Sha256).toHex(),
                     QByteArray("e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb"));
            const auto oldDebug = debug->requestId(), oldMesh = mesh->requestId();
            const auto oldHistory = history->requestId(), oldMetrics = metrics->requestId();
            done.clear();
            QVERIFY(enable->isEnabled());
            enable->trigger();
            QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000);
            QVERIFY(prepared.takeLast()[0].toBool());
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            QCOMPARE(enable->text(), QString("Enable Event"));
            QVERIFY(!debug->finish(oldDebug, {}));
            QVERIFY(!mesh->finish(oldMesh, {}, {}));
            QVERIFY(!history->finish(oldHistory, {}));
            QVERIFY(!metrics->finish(oldMetrics, {}));
            QVERIFY(!metrics->findChild<QAction *>("readRdcCounters")->isEnabled());
            exportRaw("disabled");
            const auto disabled = read("disabled.bin");
            QCOMPARE(disabled.size(), baseline.size());
            QVERIFY(disabled != baseline);
            // BGRA8 pixel (65,172): without the final Draw, the preceding
            // mapped fragment remains. This is raw storage, not display alpha.
            const auto offset = (172 * 1920 + 65) * 4;
            QCOMPARE(disabled.mid(offset, 4).toHex(), QByteArray("020606d9"));
            QCOMPARE(baseline.mid(offset, 4).toHex(), QByteArray("5dbc5eb8"));
            const auto disabledMetadata = Json::parse(read("disabled.bin.json").toStdString());
            QCOMPARE(disabledMetadata.at("missing_query_policy"), Json("isolate_readback_outputs"));
            QCOMPARE(disabledMetadata.at("frame_complete"), Json(false));
            QCOMPARE(disabledMetadata.at("experiment").at("cursor"), Json("1"));
            QVERIFY(window.saveDx12Experiment(output + "/disabled.json"));
            QCOMPARE(Json::parse(read("disabled.json").toStdString())
                         .at("ui")
                         .at("dx12")
                         .at("missing_query_policy"),
                     Json("isolate_readback_outputs"));
            done.clear();
            QVERIFY(undo->isEnabled());
            undo->trigger();
            QVERIFY(image->image().isNull());
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            exportRaw("restored");
            QCOMPARE(read("restored.bin"), baseline);
            const auto restoredMetadata = Json::parse(read("restored.bin.json").toStdString());
            QCOMPARE(restoredMetadata.at("experiment").at("cursor"), Json("0"));
            QVERIFY(restoredMetadata.at("experiment").at("document_sha256") !=
                    disabledMetadata.at("experiment").at("document_sha256"));
            QVERIFY(window.findChild<QLabel *>("frameOutputLabel")->text().contains("Incomplete"));
            // Redo/project checks do not launch additional GPU work.
            boundary->setCurrentIndex(0);
            done.clear();
            QVERIFY(redo->isEnabled());
            redo->trigger();
            QCOMPARE(enable->text(), QString("Enable Event"));
            QVERIFY(window.saveDx12Experiment(output + "/reopen.json"));
            isolation->setChecked(false);
            QVERIFY(!isolation->isChecked());
            window.openDx12Experiment(output + "/reopen.json");
            QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000);
            QVERIFY(prepared.takeLast()[0].toBool());
            QVERIFY(isolation->isChecked());
            QCOMPARE(boundary->currentIndex(), 0);
            QCOMPARE(enable->text(), QString("Enable Event"));
            auto legacy = Json::parse(read("reopen.json").toStdString());
            auto invalid = legacy;
            invalid["ui"]["dx12"]["missing_query_policy"] = "unknown";
            QFile invalidFile(output + "/invalid.json");
            QVERIFY(invalidFile.open(QIODevice::WriteOnly));
            invalidFile.write(QByteArray::fromStdString(invalid.dump()));
            invalidFile.close();
            window.openDx12Experiment(output + "/invalid.json");
            QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000);
            QVERIFY(!prepared.takeLast()[0].toBool());
            QVERIFY(isolation->isChecked());
            QCOMPARE(enable->text(), QString("Enable Event"));
            legacy["ui"]["dx12"].erase("missing_query_policy");
            QFile legacyFile(output + "/legacy.json");
            QVERIFY(legacyFile.open(QIODevice::WriteOnly));
            legacyFile.write(QByteArray::fromStdString(legacy.dump()));
            legacyFile.close();
            window.openDx12Experiment(output + "/legacy.json");
            QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000);
            QVERIFY(prepared.takeLast()[0].toBool());
            QVERIFY(!isolation->isChecked());
            QVERIFY(done.empty());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void isolatedQueryPerformanceWorkspace() {
        using Json = nlohmann::json;
        try {
            const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
            const auto output = qEnvironmentVariable("FLORA_QUERY_PERFORMANCE_UI");
            if (root.isEmpty() || output.isEmpty())
                QSKIP("Set Talos captures and a new native performance output directory");
            QVERIFY(!QFileInfo::exists(output));
            QVERIFY(QDir().mkpath(output));
            MainWindow window;
            QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
            window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
            window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
            auto locate = window.findChild<QLineEdit *>("apiLocate");
            locate->setText("19408");
            QTest::keyClick(locate, Qt::Key_Return);
            auto metrics = window.findChild<RdcCountersView *>();
            auto measure = metrics->findChild<QAction *>("readRdcCounters");
            QVERIFY(!measure->isEnabled());
            auto isolation = window.findChild<QCheckBox *>("dx12QueryIsolation");
            isolation->setChecked(true);
            QVERIFY(measure->isEnabled());
            QVERIFY(metrics->nativeMode());
            QVERIFY(!metrics->findChild<QComboBox *>("rdcCountersSource")->isEnabled());
            QVERIFY(!metrics->findChild<QAction *>("listRdcCounters")->isEnabled());
            QCOMPARE(metrics->request().at("isolate_missing_queries"), Json(true));
            done.clear();
            measure->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            QVERIFY(!window.busy());
            const auto result = metrics->result();
            const auto &measurement = result.at("native_measurement");
            QCOMPARE(result.at("execution"), Json("19408"));
            QCOMPARE(result.at("partial"), Json(true));
            QCOMPARE(result.at("frame_complete"), Json(false));
            QCOMPARE(result.at("missing_query_policy"), Json("isolate_readback_outputs"));
            QCOMPARE(result.at("request_identity").at("missing_query_policy"),
                     Json("isolate_readback_outputs"));
            QCOMPARE(measurement.at("query_outputs_incomplete"), Json(true));
            QCOMPARE(measurement.at("source"), Json("native_d3d12_queries"));
            QCOMPARE(measurement.at("source_record"), Json("192508"));
            QCOMPARE(measurement.at("pipeline_statistics_status"), Json("measured"));
            QCOMPARE(measurement.at("pipeline_statistics").at("IAVertices"), Json("828"));
            QCOMPARE(measurement.at("pipeline_statistics").at("IAPrimitives"), Json("276"));
            const auto begin = std::stoull(measurement.at("timestamp_begin").get<std::string>());
            const auto end = std::stoull(measurement.at("timestamp_end").get<std::string>());
            const auto frequency = std::stoull(measurement.at("timestamp_frequency_hz").get<std::string>());
            QVERIFY(end > begin);
            QVERIFY(frequency > 0);
            QCOMPARE(measurement.at("duration_seconds").get<double>(),
                     double(end - begin) / double(frequency));
            const auto &timeline = measurement.at("timeline");
            QCOMPARE(timeline.at("scope"), Json("frame_prefix_submitted_lists"));
            bool selected = false;
            for (const auto &entry : timeline.at("entries")) {
                const auto count = std::stoull(entry.at("execution_count").get<std::string>());
                if (!count)
                    continue;
                const auto first = std::stoull(entry.at("first_execution").get<std::string>());
                if (first <= 19408 && 19408 - first < count) {
                    selected = true;
                    QCOMPARE(entry.at("queue"), measurement.at("queue"));
                    QVERIFY(std::stoull(entry.at("timestamp_begin").get<std::string>()) <= begin);
                    QVERIFY(std::stoull(entry.at("timestamp_end").get<std::string>()) >= end);
                }
            }
            QVERIFY(selected);
            auto rows = metrics->findChild<QTreeWidget *>("rdcCounterValues");
            QCOMPARE(rows->topLevelItemCount(), 12);
            auto filter = metrics->findChild<QLineEdit *>("rdcCountersFilter");
            filter->setText("Duration");
            QVERIFY(metrics->findChild<QLabel *>("rdcCountersSummary")->text().contains("Incomplete"));
            filter->clear();
            metrics->exportResult(output + "/result.json");
            for (const auto *key : {"frame_complete", "partial"}) {
                auto invalid = result;
                invalid[key] = !result.at(key).get<bool>();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, metrics->finish(metrics->requestId(), invalid));
            }
            auto invalid = result;
            invalid["missing_query_policy"] = "fail";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, metrics->finish(metrics->requestId(), invalid));
            invalid = result;
            invalid["native_measurement"]["query_outputs_incomplete"] = false;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, metrics->finish(metrics->requestId(), invalid));
            invalid = result; invalid.erase("query_replay_coverage");
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, metrics->finish(metrics->requestId(), invalid));
            rows->setCurrentItem(rows->topLevelItem(0));
            window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
            metrics->findChild<QAction *>("locateRdcCounter")->trigger();
            QCOMPARE(
                window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                19408ULL);
            QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
            const auto old = metrics->requestId();
            isolation->setChecked(false);
            QVERIFY(metrics->result().is_null());
            QVERIFY(!metrics->finish(old, result));
            QVERIFY(!measure->isEnabled());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, metrics->finish(metrics->requestId(), result));
            invalid = result; invalid.erase("missing_query_policy");
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, metrics->finish(metrics->requestId(), invalid));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void isolatedQueryWorkspaceReplay() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_ISOLATED_QUERY_UI");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set Talos capture directory and a new query-isolation workspace evidence directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window; window.resize(1700, 1000); window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        const auto capture = root + "/Talos_2026_10_18__00_55_08.gpa_frame";
        window.openCapture(capture); QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("19410"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources"); QModelIndex selected;
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 35118) { selected = index; break; }
        }
        QVERIFY(selected.isValid()); resources->setCurrentIndex(selected);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
        auto isolation = window.findChild<QCheckBox *>("dx12QueryIsolation");
        QVERIFY(!window.findChild<QAction *>("replayFrame")->isEnabled());
        isolation->setChecked(true); done.clear(); window.replay();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QVERIFY(!window.busy());
        auto image = window.findChild<ImageView *>("frameOutput"); QCOMPARE(image->image().size(), QSize(1920, 1080));
        QVERIFY(window.findChild<QLabel *>("frameOutputLabel")->text().contains("Incomplete: missing query outputs"));
        QVERIFY(window.grab().save(output + "/workspace.png"));
        QSignalSpy exported(&window, &MainWindow::exportFinished); QTimer chooser, watchdog; watchdog.setSingleShot(true);
        bool chosen = false;
        connect(&chooser, &QTimer::timeout, &window, [&] {
            if (auto dialog = window.findChild<QFileDialog *>()) {
                chooser.stop(); chosen = true;
                dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + "/storage.bin");
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        connect(&watchdog, &QTimer::timeout, &window, [&] { if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject(); });
        chooser.start(10); watchdog.start(5000); window.findChild<QAction *>("exportOutputStorage")->trigger(); QVERIFY(chosen);
        QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        QFile binary(output + "/storage.bin"); QVERIFY(binary.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(binary.readAll(), QCryptographicHash::Sha256).toHex(),
                 QByteArray("e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb"));
        QFile sidecar(output + "/storage.bin.json"); QVERIFY(sidecar.open(QIODevice::ReadOnly));
        const auto metadata = nlohmann::json::parse(sidecar.readAll().toStdString());
        QCOMPARE(metadata.at("missing_query_policy"), nlohmann::json("isolate_readback_outputs"));
        QCOMPARE(metadata.at("frame_complete"), nlohmann::json(false));
        isolation->setChecked(false); QVERIFY(image->image().isNull());
        QVERIFY(!window.findChild<QAction *>("exportOutputStorage")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("replayFrame")->isEnabled());
        isolation->setChecked(true); window.openCapture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 30000); QVERIFY(!isolation->isChecked());
        QVERIFY(image->image().isNull());
    }
    void dx12FrameStreamReplay() {
        using Json = nlohmann::json;
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_FRAME_STREAM_OUTPUT");
        const bool reuse = qEnvironmentVariableIsSet("FLORA_DX12_FRAME_STREAM_REUSE");
        if (captures.isEmpty() || output.isEmpty())
            QSKIP("Set Talos captures and a new frame-stream replay output directory");
        if (reuse)
            QVERIFY(QFileInfo::exists(output));
        else {
            QVERIFY(!QFileInfo::exists(output));
            QVERIFY(QDir().mkpath(output));
        }
        const auto read = [&](const QString &path) {
            QFile file(output + '/' + path);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Missing frame-stream replay evidence");
            return file.readAll();
        };
        try {
            // One GPU run. The strict request must fail before creating a device;
            // explicit isolation then includes the two trailing Signals/Present.
            for (const bool isolated : {false, true}) {
                const auto name = isolated ? QString("isolated") : QString("strict");
                QStringList args{"replay", captures + "/Talos_2026_10_18__00_55_08.gpa_frame",
                                 "--id", "35118", "--max-bytes", "4294967296",
                                 "--debug-device", "--out", output + '/' + name};
                if (isolated) args.append("--isolate-missing-queries");
                QProcess process;
                process.setStandardOutputFile(output + '/' + name + ".stdout.txt");
                process.setStandardErrorFile(output + '/' + name + ".stderr.txt");
                if (!reuse) {
                    process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
                    QVERIFY(process.waitForStarted(5000));
                    QTRY_COMPARE_WITH_TIMEOUT(process.state(), QProcess::NotRunning, 180000);
                    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
                    QCOMPARE(process.exitCode(), isolated ? 0 : 2);
                }
                const auto report = Json::parse(read(name + "/report.json").toStdString());
                QCOMPARE(report.at("replay_scope"), Json("frame_stream"));
                QCOMPARE(report.at("value_time"), Json("frame_end"));
                QCOMPARE(report.at("execution"), Json("19410"));
                QCOMPARE(report.at("frame_complete"), Json(false));
                QCOMPARE(report.at("frame_stream_end_reached"), Json(isolated));
                if (!isolated) {
                    QCOMPARE(report.at("device_created"), Json(false));
                    QCOMPARE(report.at("capability_status"), Json("missing_data"));
                    continue;
                }
                QCOMPARE(report.at("completed"), Json(true));
                QCOMPARE(report.at("device").at("validation_error_count"), Json("0"));
                const auto &native = report.at("native_replay");
                QCOMPARE(native.at("last_record"), Json("192535"));
                QCOMPARE(native.at("planned_last_record"), Json("192535"));
                QCOMPARE(native.at("frame_stream_end_reached"), Json(true));
                QCOMPARE(native.at("frame_queue_schedule").at("pending_queue_operations"), Json("0"));
                const auto &presents = native.at("offscreen_presents");
                QCOMPARE(presents.size(), size_t(2));
                QCOMPARE(presents.at(0).at("record"), Json("188943"));
                QCOMPARE(presents.at(1).at("record"), Json("192535"));
                for (const auto &present : presents)
                    QCOMPARE(present.at("displayed"), Json(false));
                bool firstSignal = false, secondSignal = false;
                for (const auto &operation : native.at("frame_queue_schedule").at("first_issued_queue_operations")) {
                    firstSignal |= operation.at("record") == "192533";
                    secondSignal |= operation.at("record") == "192534";
                }
                QVERIFY(firstSignal && secondSignal);
                const auto storage = read(name + "/output_storage.bin");
                QCOMPARE(storage.size(), 8294400);
                QCOMPARE(QCryptographicHash::hash(storage, QCryptographicHash::Sha256).toHex(),
                         QByteArray("e207ad0c3ce69e174811cc7a7e1b932482c36cfd25e2bca4fac91dd48442a8cb"));
                QCOMPARE(report.at("native_texture").at("boundary"), Json("frame_end"));
                QCOMPARE(report.at("output_display").at("frame_stream_end_reached"), Json(true));
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void dx12CapturedQueryCoverage() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_QUERY_COVERAGE_OUTPUT");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set captures and a new CPU query coverage output directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        for (const auto &name : {QString("Talos_2026_10_18__00_55_08.gpa_frame"),
                                QString("bf1_2025_12_10__14_29_55.gpa_frame"),
                                QString("Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame")}) {
            dx12::StreamDirectory directory(std::filesystem::path((root + '/' + name).toStdWString()));
            dx12::SubmissionIndex submissions(directory); dx12::QueryReplayIndex queries(directory, submissions);
            const auto report = queries.information();
            QFile file(output + '/' + name + ".json"); QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray::fromStdString(report.dump(2))); file.close();
            const auto issue = queries.firstIssue(submissions.executionCount(), false);
            qInfo() << name << "query resolves/issues" << report.at("resolves_and_issues").size()
                    << "first issue" << (issue.is_null() ? QString("none") : QString::fromStdString(issue.at("reason").get<std::string>()));
            if (name.startsWith("Talos")) {
                QVERIFY(queries.firstIssue(16000, false).is_null());
                QVERIFY(queries.firstIssue(16001, true).is_null());
                const auto missing = queries.firstIssue(16001, false);
                QCOMPARE(missing.at("source_record"), nlohmann::json("188923"));
                QCOMPARE(missing.at("status"), nlohmann::json("missing_query_results"));
                QCOMPARE(missing.at("count"), nlohmann::json(236));
                QCOMPARE(missing.at("completed_count"), nlohmann::json(96));
                QCOMPARE(missing.at("missing_count"), nlohmann::json(140));
                QCOMPARE(missing.at("missing_slots_without_recorded_end"), nlohmann::json(124));
                const auto second = queries.details(19409);
                QCOMPARE(second.at("missing_count"), nlohmann::json(146));
                QCOMPARE(second.at("missing_slots_without_recorded_end"), nlohmann::json(125));
            } else QVERIFY(issue.is_null());
        }
    }
    void dx12ResourceExperimentProjects() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_RESOURCE_EXPERIMENT_OUTPUT");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set captures and a new resource experiment directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto path = std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString());
        dx12::StreamDirectory directory(path); dx12::SubmissionIndex submissions(directory); dx12::AddressIndex addresses(directory);
        const auto draw = submissions.execution(12675);
        const auto &recording = submissions.recordings().at(*submissions.recordingForRecord(draw.record));
        uint64_t rootObject{}; std::map<UINT, uint64_t> cbvs;
        for (const auto id : recording.records) {
            if (id > draw.record) break;
            const auto &p = directory.packets().at(size_t(id - 1));
            format::Reader reader(directory.payload(p));
            if (p.type == 0x1ae3c6959946eULL) { reader.skip(8); rootObject = reader.read<uint64_t>(); cbvs.clear(); }
            if (p.type == 0x1ae3cac95ac55ULL) { reader.skip(8); const auto slot = reader.read<UINT>(); cbvs[slot] = reader.read<uint64_t>(); }
        }
        QVERIFY(rootObject); uint64_t constantAddress{};
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &p = directory.packets()[i]; if (p.type != 0x127938051b445ULL) continue;
            const auto recipe = dx12::decodeRootSignature(directory.payload(p)); if (recipe.object != rootObject) continue;
            Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> reader;
            QVERIFY(SUCCEEDED(D3D12CreateVersionedRootSignatureDeserializer(recipe.bytecode.data(), recipe.bytecode.size(), IID_PPV_ARGS(&reader))));
            const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc{};
            QVERIFY(SUCCEEDED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_0, &desc)));
            for (UINT slot = 0; slot < desc->Desc_1_0.NumParameters; ++slot) {
                const auto &parameter = desc->Desc_1_0.pParameters[slot];
                if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV && !parameter.Descriptor.ShaderRegister &&
                    !parameter.Descriptor.RegisterSpace && (parameter.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL ||
                    parameter.ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX)) constantAddress = cbvs.at(slot);
            }
            break;
        }
        QVERIFY(constantAddress);
        const auto binding = addresses.resolve(constantAddress + 32, draw.record, 16);
        const auto &packet = directory.packets().at(size_t(binding.creation - 1));
        const auto recipe = dx12::decodeResourceRecipe(packet.type, directory.payload(packet));
        qInfo() << "Talos VS b0 row 2" << binding.creation << binding.object << binding.offset
                << "heap type" << int(recipe.properties.Type) << "submission" << draw.submission;
        const float row[4]{0, 0, 0, 0.25f};
        const Bytes buffer(reinterpret_cast<const uint8_t *>(row), sizeof(row));
        QFile patch(output + "/buffer.bin"); QVERIFY(patch.open(QIODevice::WriteOnly));
        patch.write(reinterpret_cast<const char *>(row), sizeof(row)); patch.close();
        const auto session = openReplaySession(path); Experiment experiment(*session);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, experiment.setResourcePatch(*session, draw.record, binding.creation, binding.offset, 0, false, buffer));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, experiment.setResourcePatch(*session, draw.submission, binding.creation, UINT64_MAX, 0, false, buffer));
        QVERIFY(!experiment.canUndo());
        experiment.setResourcePatch(*session, draw.submission, binding.creation, binding.offset, 0, false, buffer);
        experiment.save(output + "/buffer.json");
        QCOMPARE(experiment.dx12Edits(*session).resources.size(), size_t(1));
        QVERIFY(experiment.undo()); QVERIFY(experiment.dx12Edits(*session).resources.empty());
        experiment.save(output + "/restored.json"); QVERIFY(experiment.redo());
        Experiment reopened(*session); reopened.load(output + "/buffer.json", *session);
        const auto edited = reopened.dx12Edits(*session).resources.front();
        QCOMPARE(edited.offset, binding.offset); QCOMPARE(edited.submission, draw.submission);
        QCOMPARE(edited.bytes, (std::vector<uint8_t>(buffer.begin(), buffer.end())));
        // Complete layer-1 D16 data. The selected Clear will subsequently
        // overwrite it; a before-Clear boundary exposes the imported bytes.
        std::vector<uint16_t> depth(2048 * 2048, 16384);
        const Bytes texture(reinterpret_cast<const uint8_t *>(depth.data()), depth.size() * sizeof(uint16_t));
        QFile tex(output + "/texture.bin"); QVERIFY(tex.open(QIODevice::WriteOnly));
        tex.write(reinterpret_cast<const char *>(texture.data()), qint64(texture.size())); tex.close();
        Experiment textureProject(*session);
        textureProject.setResourcePatch(*session, draw.submission, 167081, 0, 1, true, texture);
        textureProject.save(output + "/texture.json");
        auto invalid = textureProject.document(); invalid["cursor"] = 0;
        invalid["history"][0]["operations"][0]["scope"] = "before_execution";
        QFile bad(output + "/invalid.json"); QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write(QByteArray::fromStdString(invalid.dump())); bad.close();
        const auto saved = reopened.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, reopened.load(bad.fileName(), *session)); QCOMPARE(reopened.document(), saved);
    }
    void dx12ModifiedResourceAnalysis() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        const auto project = qEnvironmentVariable("FLORA_DX12_MODIFIED_RESOURCE_PROJECT");
        const auto output = qEnvironmentVariable("FLORA_DX12_RESOURCE_ANALYSIS_OUTPUT");
        if (root.isEmpty() || library.isEmpty() || project.isEmpty() || output.isEmpty())
            QSKIP("Set Talos captures, RenderDoc, the constant-buffer project and a new output directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 150566) { resources->setCurrentIndex(index); break; }
        }
        QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 150566ULL);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(0);
        window.openDx12Experiment(project);
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeLast()[0].toBool());
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-vs");
        debug->setBackendPath(library, false);
        debug->findChild<QAction *>("replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 90000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QVERIFY(debug->model()); const auto model = debug->model();
        const auto trace = model->result();
        debug->exportResult(output + "/debug.json");
        QVERIFY(QFile::copy(QString::fromStdString(trace.at("capture").get<std::string>()), output + "/modified.rdc"));
        QCOMPARE(trace.at("backend"), nlohmann::json("dx12"));
        QCOMPARE(trace.at("execution"), nlohmann::json("12675"));
        QVERIFY(trace.at("gpa_modules_loaded").empty());
        QCOMPARE(model->size(), size_t(20)); // Original VS instructions, modified constant data.
        debug->findChild<QAction *>("debugStep")->trigger(); QCOMPARE(model->position(), int64_t(1));
        debug->findChild<QSpinBox *>("debugStepIndex")->setValue(int(model->size() - 1));
        model->addWatch("SV_Position.z / SV_Position.w");
        QCOMPARE(model->watchResults()[0].at("status"), nlohmann::json("available"));
        QCOMPARE(model->watchResults()[0].at("values")[0].get<double>(), 0.25);
        debug->exportResult(output + "/debug.json");
        auto history = window.findChild<ResourceHistoryView *>("resourceHistoryView");
        history->setBackendPath(library, false);
        auto read = history->findChild<QAction *>("readResourceHistory"); QVERIFY(read->isEnabled());
        done.clear(); read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        history->exportResult(output + "/history.json");
        const auto &result = history->result();
        QCOMPARE(result.at("capture"), trace.at("capture"));
        QCOMPARE(result.at("experiment_key"), trace.at("experiment_key"));
        QCOMPARE(result.at("captured_creation").at("id"), nlohmann::json("150566"));
        QVERIFY(result.at("gpa_modules_loaded").empty());
        size_t auxiliaryCopies{}, drawUses{};
        for (const auto &row : result.at("resource_history")) {
            if (row.at("usage_name") == "CopyDst" && row.at("mapping_status") == "unmapped_replay_action") {
                ++auxiliaryCopies;
                QVERIFY(!row.value("selectable", false));
                QVERIFY(!row.contains("execution") || row.at("execution").is_null());
                QCOMPARE(row.at("evidence"), nlohmann::json("recorded_operation"));
            }
            if (row.value("execution", "") == "12675") {
                ++drawUses;
                QVERIFY(row.at("usage_name").get<std::string>().ends_with("_Constants"));
                QCOMPARE(row.at("evidence"), nlohmann::json("bound_use"));
                QCOMPARE(row.at("captured_call").at("source_record"), nlohmann::json("185550"));
                QVERIFY(row.at("selectable").get<bool>());
            }
        }
        QCOMPARE(auxiliaryCopies, size_t(1)); QVERIFY(drawUses > 0);
        qInfo() << "Resource-modified VS depth 0.25; auxiliary copies" << auxiliaryCopies
                << "mapped constant uses" << drawUses << "RDC event" << trace.at("eid").get<unsigned>();
        // Undo must also invalidate analysis of resource data without a new
        // recapture merely to re-prove the already verified original output.
        const auto debugRequest = debug->requestId(), historyRequest = history->requestId();
        const auto oldHistory = result;
        window.findChild<QAction *>("undoExperiment")->trigger();
        QVERIFY(!debug->model()); QVERIFY(history->result().is_null());
        QVERIFY(!debug->finish(debugRequest, trace)); QVERIFY(!history->finish(historyRequest, oldHistory));
    }
    void dx12AnalysisCaptureReportIdentity() {
        QTemporaryDir directory;
        const nlohmann::json report{{"backend", "dx12"}, {"completed", true}, {"image_available", false},
            {"rdc_capture", "capture.rdc"}, {"execution", "9007199254740993"}, {"wide_unsigned", UINT64_MAX}};
        QFile file(directory.filePath("report.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(report.dump())); file.close();
        const auto parsed = readWorkerOutput(directory.path(), "history-capture");
        QCOMPARE(parsed.replayReport, report);
        QCOMPARE(parsed.replayReport.at("wide_unsigned").get<uint64_t>(), UINT64_MAX);
        requireDx12ExperimentReport(parsed.replayReport, nullptr);
    }
    void dx12ModifiedShaderAnalysis() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        const auto project = qEnvironmentVariable("FLORA_DX12_MODIFIED_SHADER_PROJECT");
        const auto output = qEnvironmentVariable("FLORA_DX12_MODIFIED_ANALYSIS_OUTPUT");
        if (root.isEmpty() || library.isEmpty() || project.isEmpty() || output.isEmpty())
            QSKIP("Set Talos captures, RenderDoc, the depth-0.25 VS project and a new output directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 167081) { resources->setCurrentIndex(index); break; }
        }
        QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 167081ULL);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(0);
        window.openDx12Experiment(project);
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeLast()[0].toBool());
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-vs");
        auto mesh = window.findChild<ReplayMeshView *>("replayMeshView");
        debug->setBackendPath(library, false); mesh->setBackendPath(library, false);
        auto history = window.findChild<PixelHistoryView *>("pixelHistoryView");
        history->setBackendPath(library, false);
        nlohmann::json modifiedTrace, modifiedMesh, modifiedConfig;
        uint64_t modifiedDebugRequest{}, modifiedMeshRequest{};
        for (const auto name : {QString("replaced"), QString("restored")}) {
            const bool modified = name == "replaced";
            if (!modified) {
                window.findChild<QAction *>("undoExperiment")->trigger();
                QVERIFY(!debug->model()); QVERIFY(mesh->result().is_null());
                QVERIFY(!debug->finish(modifiedDebugRequest, modifiedTrace));
                QVERIFY(!mesh->finish(modifiedMeshRequest, modifiedMesh, output));
            }
            done.clear(); debug->findChild<QAction *>("replayDebugRead")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 90000);
            QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
            QVERIFY(debug->model());
            auto model = debug->model();
            const auto &trace = model->result();
            // Preserve the recapture/trace before assertions so a failure can
            // be investigated without repeating the native GPU prefix.
            debug->exportResult(output + '/' + name + "-debug.json");
            QVERIFY(QFile::copy(QString::fromStdString(trace.at("capture").get<std::string>()), output + '/' + name + ".rdc"));
            QCOMPARE(trace.at("backend"), nlohmann::json("dx12"));
            QCOMPARE(trace.at("execution"), nlohmann::json("12675"));
            QVERIFY(trace.at("gpa_modules_loaded").empty());
            QVERIFY(model->files().empty());
            QVERIFY(model->size() > 1);
            debug->findChild<QAction *>("debugStep")->trigger(); QCOMPARE(model->position(), int64_t(1));
            debug->findChild<QSpinBox *>("debugStepIndex")->setValue(int(model->size() - 1));
            model->addWatch("SV_Position.z / SV_Position.w");
            const auto watch = model->watchResults().at(0);
            QCOMPARE(watch.at("status"), nlohmann::json("available"));
            const auto depth = watch.at("values")[0].get<double>();
            if (modified) QCOMPARE(depth, 0.25);
            else QVERIFY(std::abs(depth - 0.25) > 0.01);
            debug->exportResult(output + '/' + name + "-debug.json");
            if (modified) {
                modifiedTrace = trace; modifiedDebugRequest = debug->requestId(); modifiedConfig = model->configuration();
            } else {
                QCOMPARE(model->size(), size_t(20));
                QVERIFY(trace.at("capture") != modifiedTrace.at("capture"));
                QVERIFY(trace.at("experiment_key") != modifiedTrace.at("experiment_key"));
                QVERIFY(ReplayDebugModel::identity(trace) != ReplayDebugModel::identity(modifiedTrace));
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, model->importConfiguration(modifiedConfig));
            }
            done.clear(); mesh->findChild<QAction *>("replayMeshRead")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
            QCOMPARE(mesh->result().at("capture"), trace.at("capture"));
            QCOMPARE(mesh->result().at("experiment_key"), trace.at("experiment_key"));
            QCOMPARE(mesh->result().at("execution"), nlohmann::json("12675"));
            QCOMPARE(mesh->result().at("vertex_count"), nlohmann::json(8053));
            QCOMPARE(mesh->result().at("index_count"), nlohmann::json(42006));
            QVERIFY(mesh->result().at("gpa_modules_loaded").empty());
            auto table = mesh->findChild<QTableView *>("replayMeshPositions");
            QCOMPARE(table->model()->rowCount(), 8053);
            size_t depthChanged{};
            for (int row = 0; row < table->model()->rowCount(); ++row) {
                const auto z = table->model()->index(row, 3).data().toFloat();
                const auto w = table->model()->index(row, 4).data().toFloat();
                QVERIFY(std::isfinite(z) && std::isfinite(w)); QVERIFY(w != 0);
                if (modified) QCOMPARE(z, w * 0.25f);
                else if (z != w * 0.25f) ++depthChanged;
            }
            if (!modified) QVERIFY(depthChanged > 0);
            mesh->exportResult(output + '/' + name + "-mesh.json");
            mesh->exportResult(output + '/' + name + "-mesh.csv");
            mesh->exportResult(output + '/' + name + "-mesh.obj");
            done.clear(); history->queryPixel(12675, 4980699392ULL, 1527, 1240, 0, 1, 0);
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
            const auto historyPath = output + '/' + name + "-history.json";
            history->exportResult(historyPath);
            QFile historyFile(historyPath); QVERIFY(historyFile.open(QIODevice::ReadOnly));
            const auto historyReport = nlohmann::json::parse(historyFile.readAll().toStdString());
            QCOMPARE(historyReport.at("capture"), trace.at("capture"));
            QCOMPARE(historyReport.at("experiment_key"), trace.at("experiment_key"));
            QCOMPARE(historyReport.at("depth_event_boundaries")[1].at("post").at("raw_unorm16"),
                     nlohmann::json(modified ? 16384 : 17980));
            QVERIFY(historyReport.at("partial").get<bool>()); // Event boundaries are not per-fragment history.
            QCOMPARE(history->findChild<QTreeWidget *>("historyRecords")->topLevelItem(3)->data(0, Qt::UserRole).toULongLong(), 12675ULL);
            qInfo() << name << "steps" << model->size() << "depth" << depth << "RDC event" << trace.at("eid").get<unsigned>();
            if (modified) { modifiedMesh = mesh->result(); modifiedMeshRequest = mesh->requestId(); }
        }
    }
    void dx12ShaderReplacementPipelinePaths() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set captures for edited pipeline reconstruction");
        {
            dx12::StreamDirectory talos(std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
            const auto &packet = talos.packets().at(156846);
            const auto recipe = dx12::decodePipelineRecipe(packet.type, talos.payload(packet));
            qInfo() << "Talos depth bias" << recipe.graphics.RasterizerState.DepthBias
                    << recipe.graphics.RasterizerState.DepthBiasClamp << recipe.graphics.RasterizerState.SlopeScaledDepthBias;
            QCOMPARE(recipe.graphics.RasterizerState.SlopeScaledDepthBias, -2.0f);
        }
        // Verify native bytecode substitution for the separate compute and
        // stream-descriptor paths. These create PSOs; they submit no GPU work.
        for (const auto &[name, record] : std::vector<std::pair<QString, uint64_t>>{
                {"bf1_2025_12_10__14_29_55.gpa_frame", 4756},
                {"Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame", 1390}}) {
            dx12::StreamDirectory directory(std::filesystem::path((root + '/' + name).toStdWString()));
            const auto &packet = directory.packets().at(size_t(record - 1));
            const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
            std::vector<dx12::ShaderReplacement> edits;
            for (const auto &[stage, bytes] : recipe.shaders) {
                QCOMPARE(dx12::shaderBytecodeStage(bytes), stage);
                const auto hash = QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())), QCryptographicHash::Sha256).toHex().toStdString();
                edits.push_back({record, stage, hash, {bytes.begin(), bytes.end()}});
            }
            dx12::NativeDevice device(directory, true); dx12::NativePipelines pipelines(device, directory);
            pipelines.setShaderReplacements(std::move(edits));
            const auto report = pipelines.shaderReplacementInformation();
            QCOMPARE(report.size(), recipe.shaders.size());
            for (const auto &entry : report) {
                QCOMPARE(entry.at("original_sha256"), entry.at("replacement_sha256"));
                QCOMPARE(entry.at("pso_created"), nlohmann::json(true));
            }
            qInfo() << name << QString::fromStdString(report.dump());
            QCOMPARE(device.information().at("validation_error_count"), nlohmann::json("0"));
        }
    }
    void dx12ResourceExperimentWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto projects = qEnvironmentVariable("FLORA_DX12_RESOURCE_PROJECTS");
        const auto output = qEnvironmentVariable("FLORA_DX12_RESOURCE_WORKSPACE_OUTPUT");
        if (root.isEmpty() || projects.isEmpty() || output.isEmpty()) QSKIP("Set captures, prepared resource projects and new workspace output");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        QFile project(projects + "/buffer.json"); QVERIFY(project.open(QIODevice::ReadOnly));
        const auto document = nlohmann::json::parse(project.readAll().toStdString());
        const auto &op = document.at("history")[0].at("operations")[0];
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window; QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        const auto selectResource = [&](uint64_t id) {
            for (int row = 0; row < resources->model()->rowCount(); ++row) {
                const auto index = resources->model()->index(row, 0);
                if (index.data(Qt::UserRole).toULongLong() == id) { resources->setCurrentIndex(index); break; }
            }
            QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), id);
        };
        const auto resource = std::stoull(op.at("resource_creation").get<std::string>());
        selectResource(resource);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
        QVERIFY(window.findChild<QAction *>("patchDx12Resource")->isEnabled());
        window.patchDx12Resource(std::stoull(op.at("submission").get<std::string>()), resource,
            std::stoull(op.at("offset").get<std::string>()), 0, false, projects + "/buffer.bin");
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeLast()[0].toBool());
        // A selected buffer has no texture readback action. Import must not
        // schedule a doomed automatic output request; select an output first.
        QTest::qWait(500); QVERIFY(done.empty());
        selectResource(167081);
        window.findChild<QSpinBox *>("dx12Subresource")->setValue(1);
        auto replay = window.findChild<QAction *>("replayFrame"); QVERIFY(replay->isEnabled()); replay->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        const auto exportRaw = [&](const QString &name) {
            QTimer chooser, watchdog; watchdog.setSingleShot(true); bool chosen = false;
            QObject::connect(&chooser, &QTimer::timeout, &window, [&] {
                if (auto dialog = window.findChild<QFileDialog *>()) {
                    chooser.stop(); chosen = true;
                    dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + '/' + name + ".bin");
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            QObject::connect(&watchdog, &QTimer::timeout, &window, [&] { if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject(); });
            chooser.start(10); watchdog.start(5000); window.findChild<QAction *>("exportOutputStorage")->trigger(); QVERIFY(chosen);
        };
        exportRaw("replaced"); QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        QFile replaced(output + "/replaced.bin"); QVERIFY(replaced.open(QIODevice::ReadOnly));
        const auto bytes = replaced.readAll(); QCOMPARE(bytes.size(), 8388608);
        uint64_t written{};
        for (qsizetype i = 0; i < bytes.size(); i += 2) {
            const auto value = uint16_t(uint8_t(bytes[i])) | (uint16_t(uint8_t(bytes[i + 1])) << 8);
            QVERIFY(value == 0 || value == 16384); if (value) ++written;
        }
        QCOMPARE(written, 396850ULL);
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                 QByteArray("3f5d384f150e24e3b078e9c2aa316610a47595014f52dee9f77b5fbe2e738bcf"));
        const auto sample = (1240 * 2048 + 1527) * 2;
        QCOMPARE(uint8_t(bytes[sample]), uint8_t(0)); QCOMPARE(uint8_t(bytes[sample + 1]), uint8_t(64));
        qInfo() << "Resource patch written pixels" << written << "SHA256" << QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        QVERIFY(window.saveDx12Experiment(output + "/replaced.json"));
        done.clear(); window.findChild<QAction *>("undoExperiment")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        exportRaw("restored"); QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        QFile restored(output + "/restored.bin"); QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(restored.readAll(), QCryptographicHash::Sha256).toHex(),
                 QByteArray("c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217"));
        QVERIFY(window.saveDx12Experiment(output + "/restored.json"));
    }
    void dx12ShaderExperimentWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto projects = qEnvironmentVariable("FLORA_DX12_SHADER_PROJECTS");
        const auto output = qEnvironmentVariable("FLORA_DX12_SHADER_WORKSPACE_OUTPUT");
        if (root.isEmpty() || projects.isEmpty() || output.isEmpty()) QSKIP("Set captures, prepared shader projects and new workspace output");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        QFile project(projects + "/replaced.json"); QVERIFY(project.open(QIODevice::ReadOnly));
        const auto document = nlohmann::json::parse(project.readAll().toStdString());
        const auto &op = document.at("history")[0].at("operations")[0];
        QFile shader(output + "/replacement.dxbc"); QVERIFY(shader.open(QIODevice::WriteOnly));
        shader.write(QByteArray::fromBase64(QByteArray::fromStdString(op.at("asset").at("data").get<std::string>()))); shader.close();
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window; QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 167081) { resources->setCurrentIndex(index); break; }
        }
        QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 167081ULL);
        window.findChild<QSpinBox *>("dx12Subresource")->setValue(1);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
        QVERIFY(window.findChild<QAction *>("replaceDx12Shader")->isEnabled());
        window.replaceDx12Shader(std::stoull(op.at("pipeline").get<std::string>()), op.at("stage").get<std::string>(), shader.fileName());
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeLast()[0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        const auto exportRaw = [&](const QString &name) {
            QTimer chooser, watchdog; watchdog.setSingleShot(true); bool chosen = false;
            QObject::connect(&chooser, &QTimer::timeout, &window, [&] {
                if (auto dialog = window.findChild<QFileDialog *>()) {
                    chooser.stop(); chosen = true;
                    dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + '/' + name + ".bin");
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            QObject::connect(&watchdog, &QTimer::timeout, &window, [&] { if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject(); });
            chooser.start(10); watchdog.start(5000); window.findChild<QAction *>("exportOutputStorage")->trigger(); QVERIFY(chosen);
        };
        exportRaw("replaced"); QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        QFile replaced(output + "/replaced.bin"); QVERIFY(replaced.open(QIODevice::ReadOnly));
        const auto bytes = replaced.readAll(); QCOMPARE(bytes.size(), 8388608);
        uint64_t written{};
        for (qsizetype i = 0; i < bytes.size(); i += 2) {
            const auto value = uint16_t(uint8_t(bytes[i])) | (uint16_t(uint8_t(bytes[i + 1])) << 8);
            QVERIFY(value == 0 || value == 16384); if (value) ++written;
        }
        QVERIFY(written > 0);
        const auto sample = (1240 * 2048 + 1527) * 2;
        QCOMPARE(uint8_t(bytes[sample]), uint8_t(0)); QCOMPARE(uint8_t(bytes[sample + 1]), uint8_t(64));
        qInfo() << "Shader replacement written pixels" << written << "SHA256" << QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        QVERIFY(window.saveDx12Experiment(output + "/replaced.json"));
        done.clear(); window.findChild<QAction *>("undoExperiment")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        exportRaw("restored"); QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        QFile restored(output + "/restored.bin"); QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(restored.readAll(), QCryptographicHash::Sha256).toHex(),
                 QByteArray("c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217"));
        QVERIFY(window.saveDx12Experiment(output + "/restored.json"));
    }
    void dx12DepthHistoryProjects() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_DEPTH_HISTORY_PROJECTS");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set captures and a new output path for depth-history experiments");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto path = std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString());
        dx12::StreamDirectory directory(path);
        const auto &packet = directory.packets().at(156846);
        const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
        QCOMPARE(recipe.kind, dx12::PipelineRecipe::Kind::Graphics);
        QVERIFY(recipe.graphics.DepthStencilState.DepthEnable);
        QCOMPARE(recipe.graphics.DepthStencilState.DepthFunc, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
        QCOMPARE(recipe.graphics.DepthStencilState.DepthWriteMask, D3D12_DEPTH_WRITE_MASK_ALL);
        QCOMPARE(recipe.graphics.DSVFormat, DXGI_FORMAT_D16_UNORM);
        QCOMPARE(recipe.graphics.RasterizerState.CullMode, D3D12_CULL_MODE_NONE);
        const auto session = openReplaySession(path);
        std::vector<uint8_t> indices(84012);
        for (uint16_t i = 0; i < 6; ++i) memcpy(indices.data() + i * 2, &i, 2);
        for (const auto &name : {QString("rejected"), QString("passing")}) {
            const QByteArray source = QByteArray(R"(
                float4 main(uint id : SV_VertexID) : SV_Position {
                    uint corner = id % 3;
                    float2 p = corner == 0 ? float2(-1,-1) : corner == 1 ? float2(-1,3) : float2(3,-1);
                    return float4(p, id < 3 ? )") + (name == "rejected" ? "0.75 : 0.25" : "0.25 : 0.75") + R"(, 1);
                })";
            Microsoft::WRL::ComPtr<ID3DBlob> shader, errors;
            const auto hr = D3DCompile(source.data(), size_t(source.size()), "depth-history.hlsl", nullptr, nullptr,
                "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
            QVERIFY2(SUCCEEDED(hr), errors ? static_cast<const char *>(errors->GetBufferPointer()) : "Cannot compile depth-history VS");
            Experiment experiment(*session);
            experiment.setResourcePatch(*session, 188940, 70438, 0, 0, false, indices);
            experiment.setShader(*session, 156847, "VS", {static_cast<const uint8_t *>(shader->GetBufferPointer()), shader->GetBufferSize()});
            experiment.save(output + '/' + name + ".json");
            QFile sourceFile(output + '/' + name + ".hlsl"); QVERIFY(sourceFile.open(QIODevice::WriteOnly));
            QCOMPARE(sourceFile.write(source), source.size());
            QVERIFY(experiment.undo()); QVERIFY(experiment.undo());
            QVERIFY(experiment.dx12Edits(*session).shaders.empty());
            QVERIFY(experiment.dx12Edits(*session).resources.empty());
            experiment.save(output + '/' + name + "-restored.json");
        }
    }
    void hellbladeLensShaderProjects() {
        prepareHellbladeShaderProjects(37053, 611838, "PS");
    }
    void hellbladeIndirectShaderProjects() {
        prepareHellbladeShaderProjects(36779, 611544, "CS");
    }
  private:
    void prepareHellbladeShaderProjects(uint64_t execution, uint64_t source, const char *stage) {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_HELLBLADE_SHADER_PROJECT_OUTPUT");
        const auto shaderPath = qEnvironmentVariable("FLORA_HELLBLADE_SHADER_FILE");
        if (root.isEmpty() || output.isEmpty() || shaderPath.isEmpty())
            QSKIP("Set Hellblade2 capture, replacement DXIL and new project output paths");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto path = std::filesystem::path(
            (root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString());
        auto session = openReplaySession(path);
        dx12::StreamDirectory directory(path); dx12::SubmissionIndex submissions(directory);
        const auto selected = submissions.execution(execution);
        QCOMPARE(selected.record, source);
        const auto &recording = submissions.recordings().at(*submissions.recordingForRecord(selected.record));
        uint64_t object{}, creation{};
        for (auto id : recording.records) {
            if (id > selected.record) break;
            const auto &packet = directory.packets().at(size_t(id - 1));
            if (packet.type == 0x1ae3c82b462fcULL) {
                format::Reader reader(directory.payload(packet)); reader.skip(8);
                object = reader.read<uint64_t>(); reader.end();
            }
        }
        QVERIFY(object);
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &packet = directory.packets()[i];
            if (!dx12::isPipelineRecipe(packet.type)) continue;
            const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
            if (recipe.object != object) continue;
            QVERIFY(!creation); creation = i + 1;
        }
        QVERIFY(creation);
        QFile shader(shaderPath); QVERIFY(shader.open(QIODevice::ReadOnly));
        const auto bytes = shader.readAll();
        const format::Bytes bytecode(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()));
        QCOMPARE(dx12::shaderBytecodeStage(bytecode), std::string(stage));
        Experiment experiment(*session);
        experiment.setShader(*session, creation, stage, bytecode);
        experiment.save(output + "/modified.json");
        const auto edits = experiment.dx12Edits(*session);
        QCOMPARE(edits.shaders.size(), size_t(1));
        QVERIFY(edits.disabled.empty()); QVERIFY(edits.resources.empty());
        QVERIFY(experiment.undo()); QVERIFY(experiment.dx12Edits(*session).shaders.empty());
        experiment.save(output + "/restored.json");
        QVERIFY(experiment.redo());
        QCOMPARE(experiment.dx12Edits(*session).shaders.front().bytecode, edits.shaders.front().bytecode);
        experiment.save(output + "/redone.json");
        Experiment loaded(*session); loaded.load(output + "/modified.json", *session);
        QCOMPARE(loaded.document(), experiment.document());
        const auto &packet = directory.packets().at(size_t(creation - 1));
        auto report = dx12::describePipelineRecipe(dx12::decodePipelineRecipe(packet.type, directory.payload(packet)));
        report["pipeline_creation"] = std::to_string(creation);
        report["execution"] = std::to_string(execution); report["source_record"] = std::to_string(source);
        QFile evidence(output + "/pipeline.json"); QVERIFY(evidence.open(QIODevice::WriteOnly));
        evidence.write(QByteArray::fromStdString(report.dump(2)));
        qInfo() << "Hellblade2" << stage << "replacement pipeline" << creation << "object" << object;
    }
  private slots:
    void dx12ShaderExperimentProjects() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_SHADER_EXPERIMENT_OUTPUT");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set capture and new output paths for shader experiment preparation");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto path = std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString());
        dx12::StreamDirectory directory(path); dx12::SubmissionIndex submissions(directory);
        const auto selected = submissions.execution(12675);
        const auto &recording = submissions.recordings().at(*submissions.recordingForRecord(selected.record));
        uint64_t pipelineObject{}, pipelineRecord{};
        for (auto id : recording.records) {
            if (id > selected.record) break;
            const auto &packet = directory.packets().at(size_t(id - 1));
            if (packet.type == 0x1ae3c82b462fcULL) {
                format::Reader r(directory.payload(packet)); r.skip(8); pipelineObject = r.read<uint64_t>();
            }
        }
        QVERIFY(pipelineObject);
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &p = directory.packets()[i]; if (!dx12::isPipelineRecipe(p.type)) continue;
            const auto recipe = dx12::decodePipelineRecipe(p.type, directory.payload(p));
            if (recipe.object == pipelineObject) {
                pipelineRecord = i + 1;
                qInfo() << "Replacement target" << pipelineRecord << QString::fromStdString(dx12::describePipelineRecipe(recipe).dump());
                for (const auto &[stage, bytes] : recipe.shaders) {
                    QCOMPARE(dx12::shaderBytecodeStage(bytes), stage);
                    ID3DBlob *assembly{};
                    QVERIFY(SUCCEEDED(D3DDisassemble(bytes.data(), bytes.size(), 0, nullptr, &assembly)));
                    QFile listing(output + '/' + QString::fromStdString(stage) + ".asm"); QVERIFY(listing.open(QIODevice::WriteOnly));
                    listing.write(static_cast<const char *>(assembly->GetBufferPointer()), qint64(assembly->GetBufferSize()));
                    assembly->Release();
                }
                break;
            }
        }
        QVERIFY(pipelineRecord);
        const auto compile = [](const QByteArray &source, const char *profile) {
            ID3DBlob *shader{}, *errors{};
            const auto hr = D3DCompile(source.data(), size_t(source.size()), "replacement.hlsl", nullptr, nullptr,
                "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &shader, &errors);
            const auto error = errors ? QByteArray(static_cast<const char *>(errors->GetBufferPointer()), qsizetype(errors->GetBufferSize())) : QByteArray();
            if (errors) errors->Release();
            if (FAILED(hr)) { qWarning() << error; throw std::runtime_error(error.toStdString()); }
            std::vector<uint8_t> bytes(static_cast<const uint8_t *>(shader->GetBufferPointer()),
                                       static_cast<const uint8_t *>(shader->GetBufferPointer()) + shader->GetBufferSize());
            shader->Release(); return bytes;
        };
        const auto ps = compile("float main() : SV_Depth { return 0.5; }", "ps_5_0");
        // Preserve captured XY/W transforms and force NDC depth to 0.25. Avoid
        // the D16 half-step at 0.5 with the captured -2 slope-scale depth bias.
        // The original listing is saved as evidence for the reconstruction.
        const auto replacement = compile(R"(
            cbuffer Transform : register(b0) { float4 c[11]; };
            cbuffer Stereo : register(b1) { float4 stereo; };
            float4 main(float4 p : TEXCOORD0) : SV_Position {
                float4 world = float4(dot(p,c[5]), dot(p,c[6]), dot(p,c[7]), p.w);
                float4 clip = float4(dot(world,c[0]), dot(world,c[1]), dot(world,c[2]), dot(world,c[3]));
                clip.x = mad(stereo.x, dot(world,c[4]) - clip.x, clip.x);
                clip.z = 0.25 * clip.w;
                return clip;
            })", "vs_5_0");
        const auto incompatible = compile("cbuffer Unexpected : register(b13) { float4 p; }; float4 main() : SV_Position { return p; }", "vs_5_0");
        const auto session = openReplaySession(path); Experiment experiment(*session);
        experiment.save(output + "/baseline.json");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, experiment.setShader(*session, pipelineRecord, "VS", ps));
        QVERIFY(!experiment.canUndo());
        experiment.setShader(*session, pipelineRecord, "VS", replacement);
        QCOMPARE(experiment.dx12Edits(*session).shaders.size(), size_t(1));
        experiment.save(output + "/replaced.json");
        QVERIFY(experiment.undo()); experiment.save(output + "/restored.json");
        QVERIFY(experiment.dx12Edits(*session).shaders.empty());
        QVERIFY(experiment.redo());
        Experiment reloaded(*session); reloaded.load(output + "/replaced.json", *session);
        QCOMPARE(reloaded.dx12Edits(*session).shaders.front().bytecode, replacement);
        reloaded.setShader(*session, pipelineRecord, "VS", incompatible);
        reloaded.save(output + "/incompatible.json");
        auto invalid = experiment.document(); invalid["cursor"] = 0;
        invalid["history"][0]["operations"][0]["original_sha256"] = "wrong";
        QFile file(output + "/invalid.json"); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(invalid.dump())); file.close();
        const auto before = reloaded.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, reloaded.load(file.fileName(), *session));
        QCOMPARE(reloaded.document(), before);
        // Exercise actual DXIL mesh and pixel program headers without a GPU replay.
        dx12::StreamDirectory hellblade(std::filesystem::path((root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString()));
        const auto &meshPacket = hellblade.packets().at(1389);
        const auto mesh = dx12::decodePipelineRecipe(meshPacket.type, hellblade.payload(meshPacket));
        for (const auto &[stage, bytes] : mesh.shaders) QCOMPARE(dx12::shaderBytecodeStage(bytes), stage);
    }
    void dx12ExperimentReportIdentity() {
        const auto root = qEnvironmentVariable("FLORA_DX12_EXPERIMENT_REPORT_DIR");
        if (root.isEmpty()) QSKIP("Set the saved Talos experiment evidence directory");
        const auto read = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing experiment evidence");
            return nlohmann::json::parse(file.readAll().toStdString());
        };
        for (const auto &name : {QString("baseline"), QString("disabled"), QString("restored")}) {
            const auto report = read(root + '/' + name + "/report.json");
            const auto project = read(root + '/' + name + ".json");
            requireDx12ExperimentReport(report, project);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12ExperimentReport(report, nullptr));
            auto wrong = report; wrong["experiment"]["capture_identity"]["value"] = "another-capture";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12ExperimentReport(wrong, project));
        }
        const auto report = read(root + "/disabled/report.json");
        auto changed = read(root + "/disabled.json");
        changed["history"][0]["operations"][0]["value"] = true; // Same cursor, different edit.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12ExperimentReport(report, changed));
        requireDx12ExperimentReport({{"backend", "dx12"}}, nullptr);
        QVERIFY_THROWS_EXCEPTION(std::exception, requireDx12ExperimentReport({{"backend", "dx12"}}, changed));
    }
    void dx11ExperimentWorkspaceCompatibility() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set capture directory for the existing DX11 workspace check");
        MainWindow window; QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        const auto pixels = window.findChild<ImageView *>("frameOutput")->image().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(pixels.size(), QSize(2560, 1440));
        QCOMPARE(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(pixels.constBits()), pixels.sizeInBytes()), QCryptographicHash::Sha256).toHex(),
                 QByteArray("2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"));
        QVERIFY(window.findChild<QAction *>("openExperiment")->isEnabled());
        QVERIFY(window.findChild<QAction *>("saveExperiment")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("undoExperiment")->isEnabled());
        QVERIFY(!window.findChild<QAction *>("redoExperiment")->isEnabled());
    }
    void dx12UavBarrierWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set the BF1 capture directory for the UAV barrier workspace check");
        QTemporaryDir temp;
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), prepared(&window, &MainWindow::experimentFinished);
        QSignalSpy replayed(&window, &MainWindow::taskFinished);
        auto barrier = window.findChild<QAction *>("insertDx12UavBarrier");
        QVERIFY(barrier); QVERIFY(!barrier->isEnabled());
        window.openCapture(root + "/bf1_2025_12_10__14_29_55.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        QVERIFY(!barrier->isEnabled());
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(0);
        auto domain = window.findChild<QComboBox *>("apiEventDomain"); domain->setCurrentIndex(1);
        const auto locate = [&](const char *id) {
            auto field = window.findChild<QLineEdit *>("apiLocate"); field->setText(id); QTest::keyClick(field, Qt::Key_Return);
        };
        locate("35713"); QVERIFY(barrier->isEnabled());
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-vs");
        auto mesh = window.findChild<ReplayMeshView *>("replayMeshView");
        const auto oldDebug = debug->requestId(), oldMesh = mesh->requestId();
        barrier->trigger(); QVERIFY(window.busy()); QVERIFY(!barrier->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeFirst()[0].toBool());
        QCOMPARE(barrier->text(), QString("Remove Inserted DX12 UAV Barrier"));
        QVERIFY(!debug->finish(oldDebug, {})); QVERIFY(!mesh->finish(oldMesh, {}, {}));
        const auto project = temp.path() + "/barrier.json";
        QVERIFY(window.saveDx12Experiment(project));
        const auto readProject = [&] {
            QFile file(project); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read saved barrier experiment");
            return nlohmann::json::parse(file.readAll().toStdString());
        };
        auto saved = readProject();
        QCOMPARE(saved.at("cursor").get<int>(), 1);
        QCOMPARE(saved.at("history")[0]["operations"][0],
            (nlohmann::json{{"kind", "uav_barrier"}, {"scope", "before_execution"}, {"event", "35713"}, {"value", true}}));
        // Editing a barrier must preserve other operations, including disabled draws.
        auto enable = window.findChild<QAction *>("enableExperimentEvent"); enable->trigger();
        QCOMPARE(enable->text(), QString("Enable Event"));
        barrier->trigger(); QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 10000); QVERIFY(prepared.takeFirst()[0].toBool());
        QCOMPARE(barrier->text(), QString("Insert DX12 UAV Barrier Before Event"));
        QCOMPARE(enable->text(), QString("Enable Event"));
        QVERIFY(window.saveDx12Experiment(project)); saved = readProject();
        QCOMPARE(saved.at("cursor").get<int>(), 3);
        QCOMPARE(saved.at("history")[1]["operations"][0]["kind"].get<std::string>(), std::string("enabled"));
        QVERIFY(!saved.at("history")[2]["operations"][0]["value"].get<bool>());
        auto undo = window.findChild<QAction *>("undoExperiment"), redo = window.findChild<QAction *>("redoExperiment");
        undo->trigger(); QCOMPARE(barrier->text(), QString("Remove Inserted DX12 UAV Barrier"));
        redo->trigger(); QCOMPARE(barrier->text(), QString("Insert DX12 UAV Barrier Before Event"));
        undo->trigger(); locate("35732");
        QCOMPARE(barrier->text(), QString("Insert DX12 UAV Barrier Before Event"));
        barrier->trigger(); QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 10000); QVERIFY(prepared.takeFirst()[0].toBool());
        QVERIFY(!redo->isEnabled()); QVERIFY(window.saveDx12Experiment(project)); saved = readProject();
        QCOMPARE(saved.at("history").size(), size_t(3));
        QCOMPARE(saved.at("history")[2]["operations"][0]["event"].get<std::string>(), std::string("35732"));
        domain->setCurrentIndex(0); QVERIFY(!barrier->isEnabled());
        window.openDx12Experiment(project);
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 10000); QVERIFY(prepared.takeFirst()[0].toBool());
        QVERIFY(barrier->isEnabled()); QCOMPARE(barrier->text(), QString("Remove Inserted DX12 UAV Barrier"));
        locate("35713"); QCOMPARE(barrier->text(), QString("Remove Inserted DX12 UAV Barrier"));
        QCOMPARE(enable->text(), QString("Enable Event"));
        QVERIFY(replayed.empty()); // This test changes the project with automatic GPU replay disabled.
    }
    void dx12ExperimentWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_WORKSPACE_EXPERIMENT_OUTPUT");
        if (root.isEmpty() || output.isEmpty()) QSKIP("Set capture and new output paths for the targeted experiment workspace check");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        QSignalSpy prepared(&window, &MainWindow::experimentFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        auto domain = window.findChild<QComboBox *>("apiEventDomain"); domain->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("12675"); QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 167081) { resources->setCurrentIndex(index); break; }
        }
        QCOMPARE(resources->currentIndex().data(Qt::UserRole).toULongLong(), 167081ULL);
        window.findChild<QSpinBox *>("dx12Subresource")->setValue(1);
        auto boundary = window.findChild<QComboBox *>("outputBoundary"); boundary->setCurrentIndex(2);
        auto enable = window.findChild<QAction *>("enableExperimentEvent");
        auto undo = window.findChild<QAction *>("undoExperiment");
        auto redo = window.findChild<QAction *>("redoExperiment");
        auto image = window.findChild<ImageView *>("frameOutput");
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-vs");
        auto mesh = window.findChild<ReplayMeshView *>("replayMeshView");
        auto history = window.findChild<PixelHistoryView *>();
        auto metrics = window.findChild<RdcCountersView *>("rdcCountersView");
        const auto oldDebug = debug->requestId(), oldMesh = mesh->requestId(), oldHistory = history->requestId(), oldMetrics = metrics->requestId();
        QVERIFY(enable->isEnabled()); enable->trigger(); QVERIFY(window.busy());
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 60000); QVERIFY(prepared.takeLast()[0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(enable->text(), QString("Enable Event"));
        QCOMPARE(image->image().pixelColor(1527, 1240), QColor(0, 0, 0, 255));
        QVERIFY(!debug->finish(oldDebug, {})); QVERIFY(!mesh->finish(oldMesh, {}, {}));
        QVERIFY(!history->finish(oldHistory, {})); QVERIFY(!metrics->finish(oldMetrics, {}));
        const auto exportRaw = [&](const QString &name) {
            QTimer chooser, watchdog; watchdog.setSingleShot(true);
            bool chosen = false;
            QObject::connect(&chooser, &QTimer::timeout, &window, [&] {
                if (auto dialog = window.findChild<QFileDialog *>()) {
                    chooser.stop(); chosen = true;
                    dialog->findChild<QLineEdit *>("fileNameEdit")->setText(output + '/' + name + ".bin");
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            });
            QObject::connect(&watchdog, &QTimer::timeout, &window, [&] {
                if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject();
            });
            chooser.start(10); watchdog.start(5000);
            window.findChild<QAction *>("exportOutputStorage")->trigger();
            QVERIFY(chosen);
        };
        exportRaw("disabled"); QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        QVERIFY(window.saveDx12Experiment(output + "/disabled.json"));
        done.clear(); QVERIFY(undo->isEnabled()); undo->trigger(); QVERIFY(image->image().isNull());
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000); QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image().pixelColor(1527, 1240), QColor(70, 70, 70, 255));
        QVERIFY(!undo->isEnabled()); QVERIFY(redo->isEnabled());
        exportRaw("restored"); QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 10000); QVERIFY(exported.takeLast()[0].toBool());
        for (const auto &[name, hash] : std::map<QString, QByteArray>{{"disabled", "2daeb1f36095b44b318410b3f4e8b5d989dcc7bb023d1426c492dab0a3053e74"},
                 {"restored", "c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217"}}) {
            QFile file(output + '/' + name + ".bin"); QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.size(), 8388608); QCOMPARE(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex(), hash);
        }
        QVERIFY(window.saveDx12Experiment(output + "/restored.json"));
        window.openDx12Experiment(output + "/disabled.json");
        window.findChild<QAction *>("cancelReplay")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 10000); QVERIFY(!prepared.takeLast()[0].toBool());
        QCOMPARE(enable->text(), QString("Disable Event"));
        boundary->setCurrentIndex(0); redo->trigger(); QVERIFY(image->image().isNull());
        QCOMPARE(enable->text(), QString("Enable Event")); QVERIFY(!redo->isEnabled());
        QVERIFY(window.saveDx12Experiment(output + "/redo.json"));
        QTemporaryDir replacement;
        MainWindow reopened;
        QSignalSpy reopenedLoad(&reopened, &MainWindow::captureLoaded), reopenedProject(&reopened, &MainWindow::experimentFinished);
        reopened.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(reopenedLoad.count(), 1, 30000);
        reopened.openDx12Experiment(output + "/redo.json");
        QTRY_COMPARE_WITH_TIMEOUT(reopenedProject.count(), 1, 60000); QVERIFY(reopenedProject.takeLast()[0].toBool());
        QCOMPARE(reopened.findChild<QComboBox *>("apiEventDomain")->currentIndex(), 1);
        QCOMPARE(reopened.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(), 12675ULL);
        QCOMPARE(reopened.findChild<QTableView *>("resources")->currentIndex().data(Qt::UserRole).toULongLong(), 167081ULL);
        QCOMPARE(reopened.findChild<QSpinBox *>("dx12Subresource")->value(), 1);
        QCOMPARE(reopened.findChild<QComboBox *>("outputBoundary")->currentIndex(), 0);
        QCOMPARE(reopened.findChild<QAction *>("enableExperimentEvent")->text(), QString("Enable Event"));
        QVERIFY(reopened.saveDx12Experiment(output + "/reopened.json"));
        // A pending project must not replace the experiment belonging to another session.
        reopened.openDx12Experiment(output + "/redo.json");
        reopenedLoad.clear(); reopened.openCapture(fixture(replacement));
        QTRY_COMPARE_WITH_TIMEOUT(reopenedProject.count(), 1, 10000); QVERIFY(!reopenedProject.takeLast()[0].toBool());
        QTRY_COMPARE_WITH_TIMEOUT(reopenedLoad.count(), 1, 10000);
        QVERIFY(!reopened.findChild<QAction *>("undoExperiment")->isEnabled());
        QVERIFY(!reopened.findChild<QAction *>("saveExperiment")->isEnabled());
    }
    void talosDisableExperiment() {
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_EXPERIMENT_OUTPUT");
        if (captures.isEmpty() || output.isEmpty()) QSKIP("Set capture and new experiment output directories for this targeted GPU check");
        QVERIFY(!QFileInfo::exists(output));
        QVERIFY(QDir().mkpath(output));
        const auto capture = captures + "/Talos_2026_10_18__00_55_08.gpa_frame";
        const auto session = openReplaySession(std::filesystem::path(capture.toStdWString()));
        Experiment experiment(*session);
        experiment.save(output + "/baseline.json");
        experiment.setEnabled(*session, 12675, false);
        experiment.save(output + "/disabled.json");
        QVERIFY(experiment.undo());
        experiment.save(output + "/restored.json");
        const auto run = [&](const QString &name, nlohmann::json &report) {
            QProcess process;
            process.setProcessChannelMode(QProcess::MergedChannels);
            process.setStandardOutputFile(output + '/' + name + ".log");
            process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                {"replay", capture, "--event", "12675", "--id", "167081", "--subresource", "1",
                 "--max-bytes", "4294967296", "--debug-device", "--experiment", output + '/' + name + ".json",
                 "--out", output + '/' + name});
            if (!process.waitForStarted(5000) || !process.waitForFinished(60000)) {
                process.kill(); process.waitForFinished(5000); return false;
            }
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return false;
            QFile file(output + '/' + name + "/report.json");
            if (!file.open(QIODevice::ReadOnly)) return false;
            report = nlohmann::json::parse(file.readAll().toStdString());
            return report.at("completed").get<bool>() && report.at("device").at("validation_error_count") == "0";
        };
        nlohmann::json baseline, disabled, restored;
        QVERIFY2(run("baseline", baseline), "Baseline replay failed; inspect baseline.log");
        QVERIFY2(run("disabled", disabled), "Disabled replay failed; inspect disabled.log");
        QVERIFY2(run("restored", restored), "Undo replay failed; inspect restored.log");
        QCOMPARE(baseline.at("native_texture").at("sha256"),
                 nlohmann::json("c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217"));
        QCOMPARE(disabled.at("native_texture").at("nonzero_bytes"), nlohmann::json("0"));
        QCOMPARE(disabled.at("native_replay").at("suppressed_executions"), nlohmann::json::array({"12675"}));
        QCOMPARE(restored.at("native_replay").at("suppressed_executions"), nlohmann::json::array());
        QCOMPARE(restored.at("native_texture").at("sha256"), baseline.at("native_texture").at("sha256"));
        QVERIFY(restored.at("experiment").at("document_sha256") != disabled.at("experiment").at("document_sha256"));
        QCOMPARE(restored.at("experiment").at("capture_identity"), baseline.at("experiment").at("capture_identity"));
    }
    void hellbladePlacedAddressAliases() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty()) QSKIP("Set capture directory for Hellblade2 placed aliases");
        dx12::StreamDirectory directory(std::filesystem::path(
            (root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString()));
        dx12::AddressIndex addresses(directory);
        const auto references = addresses.candidates(0x135d940000ULL, 587987, 276);
        QCOMPARE(references.size(), size_t(3));
        std::set<uint64_t> creations;
        for (const auto &reference : references) {
            creations.insert(reference.creation);
            const auto &packet = directory.packets().at(size_t(reference.creation - 1));
            const auto recipe = dx12::decodeResourceRecipe(packet.type, directory.payload(packet));
            QCOMPARE(recipe.kind, dx12::ResourceRecipe::Kind::Placed);
            QCOMPARE(recipe.heap, 0x1f34678c0b0ULL);
            QCOMPARE(recipe.heapOffset + reference.offset, 0x7540000ULL);
        }
        QCOMPARE(creations, (std::set<uint64_t>{571474, 571692, 572348}));
        // Resource identity remains ambiguous even though a native address may
        // be proven equivalent. History/debugging must not invent one owner.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, addresses.resolve(0x135d940000ULL, 587987, 276));
        QCOMPARE(addresses.candidates(0x135d940000ULL, 587987, 40000).size(), size_t(1));
    }
    void hellbladeMeshSignature() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for the Hellblade2 mesh signature");
        const auto path = std::filesystem::path(
            (root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString());
        dx12::StreamDirectory directory(path);
        const auto recipe = dx12::decodeCommandSignature(directory.payload(directory.packets().at(1295)));
        QCOMPARE(recipe.object, 0x1f0337daf80ULL);
        QCOMPARE(recipe.rootSignature, 0ULL);
        QCOMPARE(recipe.byteStride, UINT(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS)));
        QCOMPARE(recipe.arguments.size(), size_t(1));
        QCOMPARE(recipe.arguments.front().Type, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH);
        const auto session = openReplaySession(path);
        QCOMPARE(session->executionEvent(10233).workKind, ReplayWorkKind::Dispatch);
        QCOMPARE(session->executionEvent(10267).workKind, ReplayWorkKind::MeshDispatch);
        ReplaySelection selection;
        selection.execution = 10267;
        QVERIFY(session->capability(ReplayFeature::Replay, selection).supported());
        QCOMPARE(session->capability(ReplayFeature::ShaderDebug, selection).status,
                 CapabilityStatus::NotImplemented);
        QVERIFY(session->capability(ReplayFeature::Geometry, selection).supported());
    }
    void hellbladeMeshOutputWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_MESH_OUTPUT");
        if (root.isEmpty() || output.isEmpty())
            QSKIP("Set Hellblade2 capture and saved MeshOut result directory");
        QFile file(output + "/result.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto result = nlohmann::json::parse(file.readAll().toStdString());
        QCOMPARE(result.at("execution"), nlohmann::json("10267"));
        QCOMPARE(result.at("vertex_count"), nlohmann::json(97608));
        QCOMPARE(result.at("face_count"), nlohmann::json(96731));
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        window.openCapture(root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("10267");
        QTest::keyClick(locate, Qt::Key_Return);
        auto mesh = window.findChild<ReplayMeshView *>("replayMeshView");
        QVERIFY(mesh->findChild<QAction *>("replayMeshRead")->isEnabled());
        QCOMPARE(mesh->request().at("stage"), nlohmann::json("MeshOut"));
        QCOMPARE(mesh->request().at("execution"), nlohmann::json("10267"));
        QVERIFY(!mesh->findChild<QLineEdit *>("replayMeshInstance")->isEnabled());
        QVERIFY(mesh->finish(mesh->requestId(), result, output));
        QCOMPARE(mesh->findChild<QTableView *>("replayMeshPositions")->model()->rowCount(), 97608);
        QTemporaryDir exported;
        mesh->exportResult(exported.filePath("mesh.obj"));
        QFile actual(exported.filePath("mesh.obj")), expected(output + "/post_geometry.obj");
        QVERIFY(actual.open(QIODevice::ReadOnly) && expected.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(actual.readAll(), QCryptographicHash::Sha256),
                 QCryptographicHash::hash(expected.readAll(), QCryptographicHash::Sha256));
        const auto serial = mesh->requestId();
        locate->setText("10135");
        QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(mesh->result().is_null());
        QVERIFY(!mesh->finish(serial, result, output));
        QVERIFY(!mesh->findChild<QAction *>("replayMeshRead")->isEnabled());
        locate->setText("9731");
        QTest::keyClick(locate, Qt::Key_Return);
        QCOMPARE(mesh->request().at("stage"), nlohmann::json("VSOut"));
        QVERIFY(mesh->findChild<QLineEdit *>("replayMeshInstance")->isEnabled());
    }
    void cpuFenceEventPlanning() {
        QTemporaryDir temp;
        const auto path = fixture(temp);
        QByteArray thread;
        uint64_t timestamp{};
        auto packet = [&](uint64_t type, const QByteArray &payload) {
            qword(thread, type);
            qword(thread, ++timestamp);
            qword(thread, 0);
            qword(thread, uint64_t(payload.size()));
            thread += payload;
        };
        QByteArray handle;
        qword(handle, 5);
        QByteArray registration;
        qword(registration, UINT64_MAX - 2);
        qword(registration, UINT64_MAX - 1);
        qword(registration, 5);
        dword(registration, 0);
        QByteArray draw;
        qword(draw, 7);
        for (int i = 0; i < 4; ++i)
            dword(draw, 1);
        packet(0x27000000000000ULL, handle); // Missing keyframe CPU event state.
        packet(0x114699e3dba70ULL, registration);
        packet(0x27000000000000ULL, handle); // Registration is available for native completion diagnostics.
        packet(0x1ae3c190d8b1eULL, draw);
        packet(0x27000000000000ULL, handle); // Repeated wait still requires a native submitted-queue drain.
        packet(0x1ae3c190d8b1eULL, draw);
        write(path + "/thread-7-0", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const auto plan = dx12::planCpuWaits(directory, 4);
            QCOMPARE(plan.size(), size_t(2));
            QVERIFY(plan[0].registrations.empty());
            QCOMPARE(plan[1].registrations.size(), size_t(1));
            QCOMPARE(plan[1].registrations[0].fence, UINT64_MAX - 2);
            QCOMPARE(plan[1].registrations[0].value, UINT64_MAX - 1);
            QCOMPARE(plan[1].registrations[0].registration, 2ULL);
            const auto repeated = dx12::planCpuWaits(directory, 6);
            QCOMPARE(repeated.size(), size_t(3));
            QVERIFY(repeated.back().registrations.empty()); // No invented new registration.
        }
        thread.clear();
        timestamp = 0;
        packet(0x114699e3dba70ULL, registration);
        packet(0x114699e3dba70ULL, registration);
        packet(0x27000000000000ULL, handle);
        write(path + "/thread-7-0", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const auto overlap = dx12::planCpuWaits(directory, 3);
            QCOMPARE(overlap.size(), size_t(1));
            QCOMPARE(overlap[0].registrations.size(), size_t(2));
            QCOMPARE(overlap[0].registrations[0].registration, 1ULL);
            QCOMPARE(overlap[0].registrations[1].registration, 2ULL);
        }
        registration.replace(16, 8, QByteArray(8, 0)); // Null handle is a synchronous completion wait.
        thread.clear();
        timestamp = 0;
        packet(0x27000000000000ULL, handle);
        packet(0x114699e3dba70ULL, registration);
        packet(0x1ae3c190d8b1eULL, draw);
        write(path + "/thread-7-0", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const auto plan = dx12::planCpuWaits(directory, 3);
            QCOMPARE(plan.back().record, 2ULL);
            QCOMPARE(plan.back().handle, 0ULL);
            QCOMPARE(plan.back().registrations.size(), size_t(1));
        }
        const auto bytes = [](const QByteArray &data) {
            return format::Bytes(reinterpret_cast<const uint8_t *>(data.data()), size_t(data.size()));
        };
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::decodeFenceEvent(bytes(registration.chopped(1))));
        registration[27] = char(-128); // A failed captured registration cannot supply a wait.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::decodeFenceEvent(bytes(registration)));
    }
    void hellbladePrefixDependencies() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set FLORA_DX12_CAPTURE_DIR for Hellblade2 prefix dependency inspection");
        dx12::StreamDirectory directory(std::filesystem::path(
            (root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString()));
        dx12::SubmissionIndex index(directory);
        const auto draw = index.execution(9731);
        QCOMPARE(draw.record, 577579ULL);
        QCOMPARE(draw.submission, 578777ULL);
        QCOMPARE(draw.generation, 2ULL);
        QCOMPARE(draw.ordinal, 1ULL);
        const auto waits = dx12::planCpuWaits(directory, draw.submission);
        QCOMPARE(waits.size(), size_t(2));
        QCOMPARE(waits[0].record, 577992ULL);
        QCOMPARE(waits[1].record, 578048ULL);
        QCOMPARE(waits[0].handle, 3964ULL);
        const auto next = dx12::planCpuWaits(directory, 579109);
        QCOMPARE(next.size(), size_t(4));
        QCOMPARE(next.back().record, 579052ULL);
        QCOMPARE(next.back().registrations.size(), size_t(1));
        QCOMPARE(next.back().registrations[0].registration, 578875ULL);
        QCOMPARE(next.back().registrations[0].fence, 0x1f033697240ULL);
        QCOMPARE(next.back().registrations[0].value, 449714ULL);
        const auto complete = dx12::planCpuWaits(directory, directory.packets().size());
        QCOMPARE(complete.size(), size_t(46));
        const auto overlap = std::find_if(complete.begin(), complete.end(),
                                         [](const auto &wait) { return wait.record == 595143; });
        QVERIFY(overlap != complete.end());
        QCOMPARE(overlap->registrations.size(), size_t(2));
        QCOMPARE(overlap->registrations[0].fence, 0x1f033697240ULL);
        QCOMPARE(overlap->registrations[1].fence, 0x1f033697d30ULL);
    }
    void cpuWaitPlanningAcrossThreads() {
        QTemporaryDir temp;
        const auto path = fixture(temp);
        QByteArray thread;
        auto packet = [&](uint64_t type, uint64_t timestamp, const QByteArray &payload) {
            qword(thread, type);
            qword(thread, timestamp);
            qword(thread, 0);
            qword(thread, uint64_t(payload.size()));
            thread += payload;
        };
        QByteArray wait;
        qword(wait, UINT64_MAX);
        packet(0x27000000000000ULL, 5, wait);
        QByteArray poll;
        qword(poll, 42);
        qword(poll, 99);
        packet(0x11469e993999dULL, 30, poll);
        write(path + "/thread-8-1", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const auto waits = dx12::planCpuWaits(directory, 4);
            QCOMPARE(waits.size(), size_t(1));
            QCOMPARE(waits.front().record, 1ULL);
            QCOMPARE(waits.front().handle, UINT64_MAX);
            // The two Draws belong to the other thread, so their GPU prefix
            // is retained independently from the thread's captured CPU event.
            QCOMPARE(directory.files().at(waits.front().file).thread, 8ULL);
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     dx12::planCpuWaits(directory, 4, [] { return true; }));
        }
        QByteArray draw;
        qword(draw, 7);
        for (int i = 0; i < 4; ++i)
            dword(draw, 1);
        packet(0x1ae3c190d8b1eULL, 40, draw);
        write(path + "/thread-8-1", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            QCOMPARE(dx12::planCpuWaits(directory, 4).size(), size_t(1));
            QCOMPARE(dx12::planCpuWaits(directory, 5).size(), size_t(1));
        }
        // A second stream file does not drop the earlier wait: the executor
        // must drain all submitted queues at the original wait record.
        thread.clear();
        packet(0x27000000000000ULL, 5, wait);
        write(path + "/thread-8-1", thread);
        thread.clear();
        packet(0x1ae3c190d8b1eULL, 40, draw);
        write(path + "/thread-8-2", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const auto waits = dx12::planCpuWaits(directory, 4);
            QCOMPARE(waits.size(), size_t(1));
            QCOMPARE(waits[0].record, 1ULL);
        }
        thread.clear();
        packet(0x27000000000000ULL, 5, wait.chopped(1));
        write(path + "/thread-8-1", thread);
        dx12::StreamDirectory truncated(std::filesystem::path(path.toStdWString()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::planCpuWaits(truncated, 4));
    }
    void hellbladeRasterStateDecoding() {
        // Hellblade2 records 577565-577567. Keep nullable combiner arrays and
        // 64-bit resource identity intact, and reject damaged wire lengths.
        const std::array<std::pair<uint64_t, QByteArray>, 3> samples{
            {{0x10144f9457a81ULL, QByteArray::fromHex("30f3e028f5010000000000000000803f")},
             {0x10140788f8c34ULL,
              QByteArray::fromHex("30f3e028f5010000000000000102000000000000000000000003000000")},
             {0x101407e5f6cc3ULL, QByteArray::fromHex("30f3e028f50100000000000000000000")}}};
        const auto decode = [](uint64_t type, const QByteArray &bytes) {
            return dx12::decodeArguments(
                type, {reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())});
        };
        for (const auto &[type, bytes] : samples) {
            const auto decoded = decode(type, bytes);
            QVERIFY(decoded.supported);
            QCOMPARE(decoded.consumed, size_t(bytes.size()));
            QCOMPARE(decoded.values.at("CommandList"), nlohmann::json("2152464446256"));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode(type, bytes.chopped(1)));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode(type, bytes + char(0)));
        }
        const auto bounds = decode(samples[0].first, samples[0].second).values;
        QCOMPARE(bounds.at("MinDepthBounds"), nlohmann::json(0.0));
        QCOMPARE(bounds.at("MaxDepthBounds"), nlohmann::json(1.0));
        const auto rate = decode(samples[1].first, samples[1].second).values;
        QCOMPARE(rate.at("BaseShadingRate"), nlohmann::json(0));
        QCOMPARE(rate.at("Combiners"), nlohmann::json::array({0, 3}));
        auto invalid = samples[1].second;
        invalid[13] = char(1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode(samples[1].first, invalid));
        auto nullable = samples[1].second.left(12) + char(0);
        QVERIFY(decode(samples[1].first, nullable).values.at("Combiners").is_null());
        auto image = samples[2].second.left(8);
        qword(image, UINT64_MAX);
        const auto decoded = decode(samples[2].first, image);
        QCOMPARE(decoded.resources, std::vector<uint64_t>{UINT64_MAX});
        QCOMPARE(decoded.values.at("ShadingRateImage"), nlohmann::json("18446744073709551615"));
    }
    void dx12ShaderCliExport() {
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto projects = qEnvironmentVariable("FLORA_HELLBLADE_SHADER_PROJECT_OUTPUT");
        const auto output = qEnvironmentVariable("FLORA_DX12_SHADER_EXPORT_OUTPUT");
        if (captures.isEmpty() || projects.isEmpty() || output.isEmpty())
            QSKIP("Set captures, Hellblade2 CS projects and a new shader export output directory");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto run = [&](const QString &name, const QString &capture, QStringList arguments, int expected) {
            QProcess process;
            process.setStandardOutputFile(output + '/' + name + ".stdout.txt");
            process.setStandardErrorFile(output + '/' + name + ".stderr.txt");
            arguments.prepend(captures + '/' + capture); arguments.prepend("shader");
            arguments.append({"--out", output + '/' + name});
            process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", arguments);
            if (!process.waitForStarted(5000) || !process.waitForFinished(180000)) {
                process.kill(); process.waitForFinished(5000); return false;
            }
            return process.exitStatus() == QProcess::NormalExit && process.exitCode() == expected;
        };
        const auto read = [&](const QString &name, const QString &file) {
            QFile input(output + '/' + name + '/' + file);
            if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing shader export artifact");
            return input.readAll();
        };
        const auto talos = QString("Talos_2026_10_18__00_55_08.gpa_frame");
        QVERIFY(run("dxbc", talos, {"--id", "156847", "--pipeline-stage", "vs"}, 0));
        auto report = nlohmann::json::parse(read("dxbc", "report.json").toStdString());
        QCOMPARE(report.at("format"), nlohmann::json("DXBC"));
        QCOMPARE(report.at("stage"), nlohmann::json("VS"));
        QVERIFY(report.at("disassembly").at("available").get<bool>());
        QVERIFY(!read("dxbc", "shader.asm").isEmpty());
        QCOMPARE(QCryptographicHash::hash(read("dxbc", "shader.dxbc"), QCryptographicHash::Sha256).toHex(),
                 QByteArray("81f4477676e4cb210da4408c77314811ab233c99192d3709a222fa8bdc3daec5"));
        for (const auto &bad : {QStringList{"--id", "156847", "--pipeline-stage", "CS"},
                               QStringList{"--id", "18446744073709551615", "--pipeline-stage", "VS"},
                               QStringList{"--id", "156847"}}) {
            const auto name = QString("invalid-%1").arg(QDir(output).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size());
            QVERIFY(run(name, talos, bad, 1));
            QVERIFY(!QFileInfo::exists(output + '/' + name + "/report.json"));
        }
        const auto hellblade = QString("Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame");
        for (const auto &name : {QString("modified"), QString("restored")}) {
            QVERIFY(run(name, hellblade, {"--id", "512699", "--pipeline-stage", "CS", "--experiment", projects + '/' + name + ".json"}, 0));
            report = nlohmann::json::parse(read(name, "report.json").toStdString());
            QCOMPARE(report.at("pipeline_creation"), nlohmann::json("512699"));
            QCOMPARE(report.at("pipeline_object"), nlohmann::json("2130520907296"));
            QCOMPARE(report.at("format"), nlohmann::json("DXIL"));
            QCOMPARE(report.at("stage"), nlohmann::json("CS"));
            QCOMPARE(report.at("gpu_commands_executed"), nlohmann::json(false));
            QVERIFY(report.at("disassembly").at("available").get<bool>());
            QCOMPARE(report.at("disassembly").at("backend").at("release"), nlohmann::json("v1.8.2505.1"));
            QVERIFY(!read(name, "shader.asm").isEmpty());
            QCOMPARE(report.at("replaced"), nlohmann::json(name == "modified"));
            const auto original = QByteArray("ff26fc08d0880e1ef3dbac56bad4111a6b74853fa6561aeba882c17cb2906e93");
            const auto expected = name == "modified" ? QByteArray("886d2f0680d3ef3783f814fca40fe7a1385a76448f0d85169122bb96af526378") : original;
            QCOMPARE(QCryptographicHash::hash(read(name, "shader.dxil"), QCryptographicHash::Sha256).toHex(), expected);
            QCOMPARE(report.at("original").at("sha256"), nlohmann::json(original.toStdString()));
            QCOMPARE(report.at("sha256"), nlohmann::json(expected.toStdString()));
            QCOMPARE(read(name, "report.json"), read(name, "shader.json"));
        }
    }
    void replayAllocationFailureReport() {
        const auto captures = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto output = qEnvironmentVariable("FLORA_DX12_FAILURE_REPORT_OUTPUT");
        if (captures.isEmpty() || output.isEmpty())
            QSKIP("Set capture and new output paths for the allocation failure/retry check");
        QVERIFY(!QFileInfo::exists(output)); QVERIFY(QDir().mkpath(output));
        const auto capture = captures + "/Talos_2026_10_18__00_55_08.gpa_frame";
        ReplayRequestIdentity identity{"allocation-retry", ReplayBackend::Dx12, UINT64_MAX, UINT64_MAX-1};
        const auto request = output + "/request.json";
        const auto run = [&](const QString &name, const QString &budget, int exitCode) {
            writeReplayIdentity(request, identity);
            QProcess process;
            process.setStandardOutputFile(output + '/' + name + ".stdout.txt");
            process.setStandardErrorFile(output + '/' + name + ".stderr.txt");
            process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe",
                {"replay", capture, "--event", "12675", "--id", "167081", "--subresource", "1",
                 "--max-bytes", budget, "--out", output + '/' + name, "--replay-identity", request});
            if (!process.waitForStarted(5000) || !process.waitForFinished(60000)) {
                process.kill(); process.waitForFinished(5000); return false;
            }
            return process.exitStatus() == QProcess::NormalExit && process.exitCode() == exitCode;
        };
        QVERIFY(run("failed", "1", 1));
        requireReplayIdentity(output + "/failed/response-identity.json", identity);
        const auto read = [&](const QString &name) {
            QFile file(output + '/' + name + "/report.json");
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing persisted replay report");
            return nlohmann::json::parse(file.readAll().toStdString());
        };
        const auto failed = read("failed");
        QCOMPARE(failed.at("completed"), nlohmann::json(false));
        QCOMPARE(failed.at("image_available"), nlohmann::json(false));
        QCOMPARE(failed.at("failure_stage"), nlohmann::json("initialize_transfers"));
        QCOMPARE(failed.at("allocation_limit"), nlohmann::json("1"));
        QCOMPARE(failed.at("execution"), nlohmann::json("12675"));
        QCOMPARE(failed.at("requested_resource"), nlohmann::json("167081"));
        QVERIFY(failed.at("error").get<std::string>().find("allocation budget") != std::string::npos);
        QCOMPARE(failed.at("native_replay").at("execution_boundary_reached"), nlohmann::json(false));
        QCOMPARE(failed.at("native_replay").at("resources").at("allocation_bytes"), nlohmann::json("0"));
        QVERIFY(failed.at("device").is_object());
        QVERIFY(!QFileInfo::exists(output + "/failed/frame.png"));
        --identity.request;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            requireReplayIdentity(output + "/failed/response-identity.json", identity));
        QVERIFY(run("retry", "4294967296", 0));
        requireReplayIdentity(output + "/retry/response-identity.json", identity);
        const auto retry = read("retry");
        QCOMPARE(retry.at("completed"), nlohmann::json(true));
        QVERIFY(!retry.contains("failure_stage"));
        QCOMPARE(retry.at("native_texture").at("sha256"),
            nlohmann::json("c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217"));
    }
    void workerIdentityEnvelope() {
        QTemporaryDir temp; const auto path=fixture(temp);
        const ReplayRequestIdentity expected{"test-session",ReplayBackend::Dx12,UINT64_MAX,UINT64_MAX-1};
        const auto identityPath=temp.filePath("request.json");writeReplayIdentity(identityPath,expected);
        QVERIFY(readReplayIdentity(identityPath)==expected);
        auto malformed=replayIdentityJson(expected);malformed["request"]=UINT64_MAX;
        QVERIFY_THROWS_EXCEPTION(std::exception,parseReplayIdentity(malformed));
        malformed=replayIdentityJson(expected);malformed["experiment_revision"]="18446744073709551616";
        QVERIFY_THROWS_EXCEPTION(std::exception,parseReplayIdentity(malformed));
        QProcess worker;worker.start(QCoreApplication::applicationDirPath()+"/FloraGPA.Worker.exe",
            {"inventory",path,"--out",temp.filePath("output"),"--replay-identity",identityPath});
        QVERIFY(worker.waitForFinished(15000));QCOMPARE(worker.exitStatus(),QProcess::NormalExit);
        QVERIFY2(worker.exitCode()==0,worker.readAllStandardError().constData());
        const auto response=temp.filePath("output/response-identity.json");requireReplayIdentity(response,expected);
        auto stale=expected;stale.session="new-session";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireReplayIdentity(response,stale));
        stale=expected;stale.backend=ReplayBackend::Dx11;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireReplayIdentity(response,stale));
        stale=expected;--stale.request;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireReplayIdentity(response,stale));
        stale=expected;--stale.experimentRevision;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,requireReplayIdentity(response,stale));
        stale=expected;stale.backend=ReplayBackend::Dx11;writeReplayIdentity(identityPath,stale);
        worker.start(QCoreApplication::applicationDirPath()+"/FloraGPA.Worker.exe",
            {"inventory",path,"--out",temp.filePath("wrong-backend"),"--replay-identity",identityPath});
        QVERIFY(worker.waitForFinished(15000));QCOMPARE(worker.exitCode(),1);
        QVERIFY(worker.readAllStandardError().contains("backend does not match"));
        QVERIFY(!QFile::exists(temp.filePath("wrong-backend/response-identity.json")));
        // Exercise analysis-envelope publication without loading a GPU backend.
        const nlohmann::json analysis{{"action","identity-test-invalid-action"},
            {"out",temp.filePath("analysis").toStdString()}, {"request_identity",replayIdentityJson(stale)}};
        write(temp.filePath("analysis-job.json"),QByteArray::fromStdString(analysis.dump()));
        worker.start(QCoreApplication::applicationDirPath()+"/FloraGPA.Rdc.exe",{"--job",temp.filePath("analysis-job.json")});
        QVERIFY(worker.waitForFinished(15000));QCOMPARE(worker.exitCode(),1);
        requireReplayIdentity(temp.filePath("analysis/response-identity.json"),stale);
        auto isolated = expected; isolated.isolateMissingQueries = true;
        QCOMPARE(replayIdentityJson(expected).at("schema"), nlohmann::json(1));
        QCOMPARE(replayIdentityJson(isolated).at("schema"), nlohmann::json(2));
        QVERIFY(parseReplayIdentity(replayIdentityJson(isolated)) == isolated);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireReplayIdentity(response, isolated));
        malformed = replayIdentityJson(isolated); malformed["backend"] = "dx11";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, parseReplayIdentity(malformed));
        malformed = replayIdentityJson(isolated); malformed["missing_query_policy"] = "unknown";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, parseReplayIdentity(malformed));
        writeReplayIdentity(identityPath, isolated);
        worker.start(QCoreApplication::applicationDirPath()+"/FloraGPA.Worker.exe",
            {"replay", path, "--event", "1", "--out", temp.filePath("wrong-policy"), "--replay-identity", identityPath});
        QVERIFY(worker.waitForFinished(15000)); QCOMPARE(worker.exitCode(), 1);
        QVERIFY(worker.readAllStandardError().contains("query policy does not match"));
        QVERIFY(!QFile::exists(temp.filePath("wrong-policy/response-identity.json")));
        auto isolatedAnalysis = analysis;
        isolatedAnalysis["out"] = temp.filePath("isolated-analysis").toStdString();
        isolatedAnalysis["request_identity"] = replayIdentityJson(isolated);
        write(temp.filePath("isolated-analysis-job.json"), QByteArray::fromStdString(isolatedAnalysis.dump()));
        worker.start(QCoreApplication::applicationDirPath()+"/FloraGPA.Rdc.exe", {"--job",temp.filePath("isolated-analysis-job.json")});
        QVERIFY(worker.waitForFinished(15000)); QCOMPARE(worker.exitCode(), 1);
        QVERIFY(worker.readAllStandardError().contains("Unsupported query-isolated analysis"));
        requireReplayIdentity(temp.filePath("isolated-analysis/response-identity.json"), isolated);
    }
    void isolatedQueryReportPolicy() {
        nlohmann::json strict{{"backend", "dx12"}, {"frame_complete", false},
            {"native_replay", {{"frame_replayed", false}}}};
        requireDx12QueryPolicyReport(strict, false); // Legacy strict reports remain readable.
        QVERIFY_THROWS_EXCEPTION(std::exception, requireDx12QueryPolicyReport(strict, true));
        auto isolated = strict;
        isolated["missing_query_policy"] = "isolate_readback_outputs";
        isolated["native_replay"]["missing_query_policy"] = "isolate_readback_outputs";
        isolated["native_replay"]["query_readback_availability_valid"] = true;
        isolated["output_display"] = {{"missing_query_policy", "isolate_readback_outputs"}};
        requireDx12QueryPolicyReport(isolated, true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12QueryPolicyReport(isolated, false));
        auto invalid = isolated; invalid["native_replay"]["query_readback_availability_valid"] = false;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12QueryPolicyReport(invalid, true));
        invalid = isolated; invalid["frame_complete"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12QueryPolicyReport(invalid, true));
        invalid = isolated; invalid["output_display"]["missing_query_policy"] = "fail";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireDx12QueryPolicyReport(invalid, true));
    }
    void packetIdentityAndCapabilities() {
        QTemporaryDir temp; const auto path=fixture(temp);
        auto session=openReplaySession(std::filesystem::path(path.toStdWString()));
        QCOMPARE(session->backend(),ReplayBackend::Dx12);
        QCOMPARE(session->events().size(),size_t(2));
        QCOMPARE(*session->event(1).fileOffset,uint64_t(0));
        QCOMPARE(*session->event(2).fileOffset,uint64_t(60));
        const auto args = session->eventDetails(1).at("arguments");
        QCOMPARE(args.at("CommandList").get<std::string>(),std::string("1311768467463790320"));
        QCOMPARE(args.at("BaseVertexLocation").get<int>(),-3);
        QCOMPARE(args.at("IndexCountPerInstance").get<unsigned>(),36U);
        QVERIFY(session->event(1).work);
        QVERIFY(!session->event(1).submission);
        QVERIFY(!session->dx11Frame());
        QVERIFY(session->capability(ReplayFeature::ApiInspection).supported());
        QVERIFY(!session->capability(ReplayFeature::Replay).supported());
        ReplaySelection other; other.session="other";
        QCOMPARE(session->capability(ReplayFeature::ApiInspection,other).status,CapabilityStatus::Failed);
        auto second=openReplaySession(std::filesystem::path(path.toStdWString()));
        QVERIFY(second->identity()!=session->identity());
    }
    void legacyExperimentIdentity() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("External DX11 capture is not configured");
        Frame frame(std::filesystem::path((root+"/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString()));
        Experiment experiment(frame);QTemporaryDir temp;const auto path=temp.filePath("experiment.json");
        auto legacy=experiment.document();legacy.erase("backend");legacy.erase("capture_identity");
        write(path,QByteArray::fromStdString(legacy.dump()));experiment.load(path,frame);
        QCOMPARE(experiment.document().at("backend").get<std::string>(),std::string("dx11"));
        QCOMPARE(experiment.document().at("capture_identity").at("value").get<std::string>(),frame.sha256());
        const auto before=experiment.document();auto invalid=before;invalid["backend"]="dx12";
        write(path,QByteArray::fromStdString(invalid.dump()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,experiment.load(path,frame));QVERIFY(experiment.document()==before);
        invalid=before;invalid["capture_identity"]["value"]="different-capture";
        write(path,QByteArray::fromStdString(invalid.dump()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,experiment.load(path,frame));QVERIFY(experiment.document()==before);
    }
    void malformedAndCancellation() {
        QTemporaryDir temp; const auto path=fixture(temp);
        const auto native=std::filesystem::path(path.toStdWString());
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,openReplaySession(native,[]{return true;}));
        write(path+"/thread-7-0",QByteArray(31,0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,openReplaySession(native));
        QByteArray bad; qword(bad,1);qword(bad,1);qword(bad,0);qword(bad,UINT64_MAX);
        write(path+"/thread-7-0",bad);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,openReplaySession(native));
        QVERIFY(QFile::remove(path+"/resources"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,openReplaySession(native));
    }
    void resourceOffsetAboveFourGiB() {
        QTemporaryDir temp; const auto name=temp.path()+"/sparse";
        const auto native=std::filesystem::path(name.toStdWString());
        HANDLE file=CreateFileW(native.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(file!=INVALID_HANDLE_VALUE);
        DWORD done{}; const bool sparse=DeviceIoControl(file,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&done,nullptr)!=0;
        if (!sparse) { CloseHandle(file); QSKIP("Filesystem does not support sparse files"); }
        LARGE_INTEGER offset{};offset.QuadPart=(1LL<<32)+17;
        const bool positioned=SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)!=0;
        const char sentinel='X';const bool written=positioned&&WriteFile(file,&sentinel,1,&done,nullptr)&&done==1;
        CloseHandle(file);QVERIFY(written);
        dx12::MappedFile mapped(native);
        QCOMPARE(mapped.bytes(uint64_t(offset.QuadPart),1)[0],uint8_t('X'));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,mapped.bytes(mapped.size(),1));
    }
    void texturePixelSubresource() {
        nlohmann::json d{{"Dimension", 3}, {"Width", "512"}, {"Height", 4},
            {"DepthOrArraySize", 3}, {"MipLevels", 2}, {"SampleCount", 1}};
        QCOMPARE(dx12TexturePixelSubresource(d, 200, 1, 1, 2, 0), 5U);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 256, 1, 1, 2, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 200, 1, 2, 2, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 200, 1, 1, 3, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 200, 1, 1, 2, 1));
        d["MipLevels"] = 0;
        QCOMPARE(dx12TexturePixelSubresource(d, 200, 1, 1, 2, 0), 21U);
        d["Dimension"] = 2; d["Height"] = 1;
        QCOMPARE(dx12TexturePixelSubresource(d, 200, 0, 1, 2, 0), 21U);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 200, 1, 1, 2, 0));
        d["Dimension"] = 4;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 200, 0, 1, 2, 0));
        d["Dimension"] = 3; d["Width"] = "512extra";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12TexturePixelSubresource(d, 200, 0, 1, 2, 0));
    }
    void textureStorePixelWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto reportPath = qEnvironmentVariable("FLORA_TEXTURE_STORE_REPORT");
        if (root.isEmpty() || reportPath.isEmpty()) QSKIP("Set Hellblade2 capture and actual texture-store report");
        QFile file(reportPath); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report = nlohmann::json::parse(file.readAll().toStdString());
        MainWindow window; window.resize(1550, 950); window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        window.findChild<QSpinBox *>("dx12AllocationMiB")->setValue(12288);
        auto locate = window.findChild<QLineEdit *>("apiLocate"); locate->setText("36779"); QTest::keyClick(locate, Qt::Key_Return);
        auto view = window.findChild<ReplayDebugView *>("replayDebug-cs"); QVERIFY(view);
        QVERIFY(view->finish(view->requestId(), report));
        auto tree = view->findChild<QTreeWidget *>("debugTextureStores"); QVERIFY(tree);
        auto boundary = window.findChild<QComboBox *>("outputBoundary"); boundary->setCurrentIndex(1);
        auto subresource = window.findChild<QSpinBox *>("dx12Subresource"); subresource->setValue(7);
        const auto select = [&] {
            return QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
                Q_ARG(QTreeWidgetItem *, tree->topLevelItem(1)), Q_ARG(int, 2));
        };
        QVERIFY(select()); QCOMPARE(boundary->currentIndex(), 1); QCOMPARE(subresource->value(), 0);
        QVERIFY(view->model());
        auto history = window.findChild<PixelHistoryView *>("pixelHistoryView"); QVERIFY(history);
        QCOMPARE(history->request().at("x"), nlohmann::json(416));
        QCOMPARE(history->request().at("y"), nlohmann::json(296));
        QCOMPARE(history->request().at("resource_creation"), nlohmann::json("570673"));
        boundary->setCurrentIndex(2); QVERIFY(select());
        auto image = window.findChild<ImageView *>("frameOutput"); QVERIFY(image->image().isNull());
        window.replay(); QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 120000);
        QVERIFY2(done.takeLast()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
        QVERIFY(!image->image().isNull()); QCOMPARE(image->selectedPixel(), std::optional<QPoint>(QPoint(416, 296)));
        const auto &extent = report.at("texture_stores").at("rows")[1].at("view_extent");
        QCOMPARE(image->image().size(), QSize(extent[0].get<int>(), extent[1].get<int>()));
        const auto previous = image->image();
        view->texturePixelRequested("stale-context", 36779, 571530, 2149346282688ULL, 832, 592, 0, 0, 0);
        QCOMPARE(image->image(), previous); QCOMPARE(image->selectedPixel(), std::optional<QPoint>(QPoint(416, 296)));
        const auto screenshot = qEnvironmentVariable("FLORA_DX12_UI_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        subresource->setValue(1); QVERIFY(image->image().isNull()); QVERIFY(!image->selectedPixel());
    }
    void workspaceAndSearch() {
        QTemporaryDir temp; const auto path=fixture(temp);
        MainWindow window;
        QSignalSpy loaded(&window,&MainWindow::captureLoaded);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,10000);
        QVERIFY(!window.busy());
        auto view=window.findChild<QTableView *>("apiLog");QVERIFY(view);
        QCOMPARE(view->model()->rowCount(),2);
        auto search=window.findChild<QLineEdit *>("apiSearch");QVERIFY(search);
        search->setText("DrawIndexedInstanced");QCOMPARE(view->model()->rowCount(),2);
        search->setText("BaseVertexLocation -3");QCOMPARE(view->model()->rowCount(),2);
        search->setText("not-a-call");QCOMPARE(view->model()->rowCount(),0);
        auto locate=window.findChild<QLineEdit *>("apiLocate");QVERIFY(locate);
        locate->setText("2");QTest::keyClick(locate,Qt::Key_Return);
        QCOMPARE(view->currentIndex().data(Qt::UserRole).toULongLong(),2ULL);
        QVERIFY(search->text().isEmpty());
        auto thread=window.findChild<QLineEdit *>("apiThreadFilter");QVERIFY(thread);
        thread->setText("8");QCOMPARE(view->model()->rowCount(),0);
        thread->setText("7");QCOMPARE(view->model()->rowCount(),2);
        window.openCapture(path);QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),2,10000);
        QVERIFY(!window.busy());
    }
    void selectedReplayWorkspace() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for selected native replay workspace checks");
        MainWindow window; window.resize(1500,900); window.show();
        QSignalSpy loaded(&window,&MainWindow::captureLoaded), done(&window,&MainWindow::taskFinished);
        window.openCapture(root+"/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        auto replay=window.findChild<QAction *>("replayFrame"); QVERIFY(replay); QVERIFY(!replay->isEnabled());
        auto domain=window.findChild<QComboBox *>("apiEventDomain"); QVERIFY(domain); domain->setCurrentIndex(1);
        auto locate=window.findChild<QLineEdit *>("apiLocate"); QVERIFY(locate);
        locate->setText("12675"); QTest::keyClick(locate,Qt::Key_Return);
        auto resources=window.findChild<QTableView *>("resources"); QVERIFY(resources);
        QModelIndex depth;
        for(int row=0;row<resources->model()->rowCount();++row) {
            auto index=resources->model()->index(row,0);
            if(index.data(Qt::UserRole).toULongLong()==167081) { depth=index; break; }
        }
        QVERIFY(depth.isValid()); resources->setCurrentIndex(depth);
        auto sub=window.findChild<QSpinBox *>("dx12Subresource"); QVERIFY(sub); sub->setValue(1);
        auto boundary=window.findChild<QComboBox *>("outputBoundary"); QVERIFY(boundary); boundary->setCurrentIndex(1);
        auto image=window.findChild<ImageView *>("frameOutput"); QVERIFY(image);
        QVERIFY(replay->isEnabled()); replay->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),60000); QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image().size(),QSize(2048,2048)); QCOMPARE(image->image().pixelColor(1527,1240),QColor(0,0,0,255));
        boundary->setCurrentIndex(2); QVERIFY(image->image().isNull());
        done.clear(); window.replay(); QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),60000); QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(image->image().size(),QSize(2048,2048)); QCOMPARE(image->image().pixelColor(1527,1240),QColor(70,70,70,255));
        QVERIFY(window.findChild<QAction *>("exportOutputStorage")->isEnabled());
        const auto screenshot=qEnvironmentVariable("FLORA_DX12_UI_SCREENSHOT");
        if(!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        // Replacing a running DX12 job must not publish into the next session.
        done.clear(); window.replay(); QTemporaryDir temp; const auto replacement=fixture(temp);
        loaded.clear(); window.openCapture(replacement); QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        QVERIFY(!window.busy()); QVERIFY(image->image().isNull());
        // The common report/image route must still accept the existing DX11 baseline.
        done.clear(); window.openCapture(root+"/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),60000); QVERIFY(done.takeLast()[0].toBool());
        const auto pixels=image->image().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(pixels.size(),QSize(2560,1440));
        QCOMPARE(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(pixels.constBits()),pixels.sizeInBytes()),QCryptographicHash::Sha256).toHex(),
            QByteArray("2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"));
        QVERIFY(window.findChild<QComboBox *>("outputTarget")->isEnabled()); QVERIFY(!sub->isVisible());
    }
    void indirectSignatureRecipes() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for BF1 command signature records");
        dx12::StreamDirectory directory(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        const std::array<std::tuple<size_t, UINT, D3D12_INDIRECT_ARGUMENT_TYPE>, 3> cases{
            {{15483, 12, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH},
             {15525, 16, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW},
             {15543, 20, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED}}};
        for (const auto &[record, stride, type] : cases) {
            const auto bytes = directory.payload(directory.packets()[record - 1]);
            const auto recipe = dx12::decodeCommandSignature(bytes);
            QCOMPARE(recipe.byteStride, stride);
            QCOMPARE(recipe.arguments.size(), size_t(1));
            QCOMPARE(recipe.arguments[0].Type, type);
            QCOMPARE(recipe.rootSignature, 0ULL);
            QCOMPARE(recipe.nodeMask, 1U);
            QVERIFY(recipe.object);
            QCOMPARE(recipe.result, S_OK);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     dx12::decodeCommandSignature(bytes.first(bytes.size() - 1)));
            auto invalid = std::vector<uint8_t>(bytes.begin(), bytes.end());
            const UINT wrong = 2;
            memcpy(invalid.data() + 13, &wrong, 4);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::decodeCommandSignature(invalid));
        }
    }
    void forwardConsumedViews() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for the native descriptor fixture device");
        dx12::StreamDirectory original(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        dx12::NativeDevice device(original, true);
        for (bool ambiguous : {false, true}) {
            QTemporaryDir temp;
            const auto path = fixture(temp);
            QByteArray packets;
            auto packet = [&](uint64_t type, const QByteArray &body) {
                qword(packets, type);
                qword(packets, 0);
                qword(packets, 0);
                qword(packets, body.size());
                packets += body;
            };
            QByteArray increment;
            qword(increment, 1);
            dword(increment, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            dword(increment, 32);
            packet(0x127932ffcb166ULL, increment);
            QByteArray heap;
            qword(heap, 1);
            heap.append(char(1));
            const D3D12_DESCRIPTOR_HEAP_DESC description{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4,
                                                         D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            heap.append(reinterpret_cast<const char *>(&description), sizeof(description));
            const auto iid = __uuidof(ID3D12DescriptorHeap);
            heap.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
            heap.append(char(1));
            qword(heap, 1);
            qword(heap, 2);
            dword(heap, S_OK);
            packet(0x127932eb2eb14ULL, heap);
            QByteArray start;
            qword(start, 2);
            start.append(char(1));
            qword(start, 0x1000);
            packet(0x15a3eb3b8e877ULL, start);
            packet(0x1ae3c7884148bULL, {}); // Boundary only; not submitted in this descriptor fixture.
            for (unsigned copy = 0; copy < 2; ++copy) {
                QByteArray view;
                qword(view, 1);
                qword(view, 0);
                view.append(char(1));
                D3D12_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = ambiguous && copy ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                view.append(reinterpret_cast<const char *>(&desc), sizeof(desc));
                qword(view, 0x1000);
                packet(0x1279306e469b5ULL, view);
            }
            QFile file(path + "/metadata");
            QVERIFY(file.open(QIODevice::ReadOnly));
            auto metadata = file.readAll();
            file.close();
            qword(metadata, 3);
            qword(metadata, 0);
            qword(metadata, 0);
            qword(metadata, 0x288 + 40 + packets.size());
            dword(metadata, 0);
            dword(metadata, 0);
            metadata += packets;
            write(path + "/metadata", metadata);
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            dx12::NativeResources resources(device, directory, 1024 * 1024);
            dx12::NativeDescriptors descriptors(device, directory, resources);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     descriptors.consumedCpu(0x1020, 4, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
            if (ambiguous) {
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         descriptors.consumedCpu(0x1000, 4, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
                QCOMPARE(descriptors.information().at("forward_cpu_view_restores"), nlohmann::json("0"));
            } else {
                // A frame reference cannot trigger keyframe forward restoration.
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         descriptors.consumedCpu(0x1000, 7, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
                QVERIFY(descriptors.consumedCpu(0x1000, 4, D3D12_DESCRIPTOR_HEAP_TYPE_RTV).ptr);
                QCOMPARE(descriptors.information().at("forward_cpu_view_restores"), nlohmann::json("1"));
                QVERIFY(descriptors.consumedCpu(0x1000, 4, D3D12_DESCRIPTOR_HEAP_TYPE_RTV).ptr);
                QCOMPARE(descriptors.information().at("forward_cpu_view_restores"), nlohmann::json("1"));
            }
        }
        QCOMPARE(device.information().at("validation_error_count"), nlohmann::json("0"));
    }
    void uavClearReplay() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for the BF1 UAV clear");
        dx12::StreamDirectory directory(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        uint64_t creation{};
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &packet = directory.packets()[i];
            if (!dx12::isResourceRecipe(packet.type))
                continue;
            const auto recipe = dx12::decodeResourceRecipe(packet.type, directory.payload(packet));
            if (recipe.object == 0x20664cf30ULL) {
                QVERIFY(!creation);
                creation = i + 1;
                QCOMPARE(recipe.description.Dimension, D3D12_RESOURCE_DIMENSION_BUFFER);
                QVERIFY(recipe.description.Width >= 4700 * 4);
            }
        }
        QVERIFY(creation);
        dx12::NativeDevice device(directory, true);
        dx12::NativeReplay replay(device, directory, 4ULL * 1024 * 1024 * 1024);
        replay.prepareExecution(35703, true);
        replay.initializeTransfers();
        replay.replayTo(35703, true);
        const auto result = replay.inspectBuffer(creation, 0, 4700 * 4);
        QByteArray expected;
        const float one = 1.0f;
        for (unsigned i = 0; i < 4700; ++i)
            expected.append(reinterpret_cast<const char *>(&one), 4);
        QCOMPARE(result.at("sha256"),
                 nlohmann::json(
                     QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex().toStdString()));
        QCOMPARE(result.at("execution"), nlohmann::json("35703"));
        qInfo() << "BF1 cleared float buffer" << QString::fromStdString(result.dump());
        QCOMPARE(device.information().at("validation_error_count"), nlohmann::json("0"));
    }
    void keyframeRecordingReplay() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for the BF1 keyframe-recorded Draw");
        dx12::StreamDirectory directory(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        dx12::SubmissionIndex index(directory);
        const bool afterEvent = qEnvironmentVariable("FLORA_DX12_BOUNDARY") != "before";
        const auto draw = index.execution(21622);
        QCOMPARE(draw.record, 328782ULL);
        QVERIFY(draw.record < directory.initializationCount());
        QVERIFY(draw.submission > directory.initializationCount());
        dx12::AddressIndex addresses(directory);
        const auto uav = addresses.resolve(0x0f81261c, 328780, 4);
        bool shaderFound = false;
        for (size_t i = 0; i < directory.initializationCount(); ++i) {
            const auto &packet = directory.packets()[i];
            if (!dx12::isPipelineRecipe(packet.type))
                continue;
            const auto recipe = dx12::decodePipelineRecipe(packet.type, directory.payload(packet));
            if (recipe.object != 0x34145ad0)
                continue;
            for (const auto &[stage, bytes] : recipe.shaders)
                if (stage == "VS") {
                    ID3DBlob *assembly{};
                    QVERIFY(SUCCEEDED(D3DDisassemble(bytes.data(), bytes.size(), 0, nullptr, &assembly)));
                    const auto text = QByteArray(static_cast<const char *>(assembly->GetBufferPointer()),
                                                 qsizetype(assembly->GetBufferSize()));
                    assembly->Release();
                    qInfo() << "BF1 first Draw VS" << text;
                    QVERIFY(text.contains("store_structured u0.x, l(0), l(0), l(1)"));
                    shaderFound = true;
                }
            break;
        }
        QVERIFY(shaderFound);
        dx12::NativeDevice device(directory, true);
        dx12::NativeReplay replay(device, directory, 4ULL * 1024 * 1024 * 1024);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.prepareExecution(UINT64_MAX, true));
        replay.prepareExecution(21622, afterEvent);
        replay.initializeTransfers();
        const auto initialized = replay.information();
        QCOMPARE(initialized.at("recorded_commands").value("ID3D12GraphicsCommandListDrawInstanced", 0),
                 afterEvent ? 1 : 0);
        QVERIFY(!initialized.at("executed_commands").contains("ID3D12GraphicsCommandListDrawInstanced"));
        const auto before = replay.inspectBuffer(uav.creation, uav.offset, 4);
        replay.replayTo(21622, afterEvent);
        const auto after = replay.inspectBuffer(uav.creation, uav.offset, 4);
        qInfo() << "BF1 root UAV" << uav.creation << uav.object << uav.offset << "before"
                << QString::fromStdString(before.dump()) << "after" << QString::fromStdString(after.dump());
        QCOMPARE(
            replay.information().at("executed_commands").value("ID3D12GraphicsCommandListDrawInstanced", 0),
            afterEvent ? 1 : 0);
        QVERIFY(replay.information().at("execution_boundary_reached").get<bool>());
        QCOMPARE(after.at("boundary"), nlohmann::json(afterEvent ? "after_event" : "before_event"));
        QCOMPARE(after.at("execution"), nlohmann::json("21622"));
        QCOMPARE(before.at("preview_hex"), nlohmann::json("00000000"));
        QCOMPARE(after.at("preview_hex"), nlohmann::json(afterEvent ? "01000000" : "00000000"));
        QCOMPARE(device.information().at("validation_error_count"), nlohmann::json("0"));
    }
    void pixelHistoryIdentityAndMapping() {
        const auto path = qEnvironmentVariable("FLORA_DX12_HISTORY");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_HISTORY to the Talos D16 history result");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        auto report = nlohmann::json::parse(input.readAll().toStdString());
        PixelHistoryView history;
        history.setContext("wide-history", 9007199254740993ULL, true);
        history.setDx12Resource(UINT64_MAX - 1, UINT64_MAX);
        QCOMPARE(history.request().at("execution"), nlohmann::json("9007199254740993"));
        QCOMPARE(history.request().at("resource"), nlohmann::json("18446744073709551615"));
        QCOMPARE(history.request().at("resource_creation"), nlohmann::json("18446744073709551614"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, history.finish(history.requestId(), report));
        report["execution"] = "9007199254740993";
        report["history"][0]["execution"] = "18446744073709551615";
        report["history"][1]["selectable"] = false;
        report["history"][1]["mapping_status"] = "ambiguous";
        QVERIFY(history.finish(history.requestId(), report));
        auto rows = history.findChild<QTreeWidget *>("historyRecords");
        QSignalSpy jump(&history, &PixelHistoryView::eventRequested);
        rows->setCurrentItem(rows->topLevelItem(0));
        history.findChild<QAction *>("locateHistoryEvent")->trigger();
        QCOMPARE(jump.count(), 1);
        QCOMPARE(jump[0][0].toULongLong(), UINT64_MAX);
        rows->setCurrentItem(rows->topLevelItem(1));
        QVERIFY(!history.findChild<QAction *>("locateHistoryEvent")->isEnabled());
        report["history"][0]["execution"] = UINT64_MAX;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, history.finish(history.requestId(), report));
        QCOMPARE(rows->topLevelItem(0)->data(0, Qt::UserRole).toULongLong(), UINT64_MAX);
        const auto serial = history.requestId();
        history.setDx12Resource(12, 34);
        QVERIFY(!history.finish(serial, report));
        QCOMPARE(rows->topLevelItemCount(), 0);
        history.setContext("dx11", 1);
        history.selectPixel(84, 0, 0, 0, 0, 0);
        QVERIFY(!history.request().contains("execution"));
        QCOMPARE(history.request().at("resource"), nlohmann::json(84));
    }
    void selectedPixelHistoryWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        if (root.isEmpty() || library.isEmpty())
            QSKIP("Set capture directory and RenderDoc library for DX12 pixel history UI");
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("12675");
        QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        QModelIndex depth;
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 167081) {
                depth = index;
                break;
            }
        }
        QVERIFY(depth.isValid());
        resources->setCurrentIndex(depth);
        window.findChild<QSpinBox *>("dx12Subresource")->setValue(1);
        window.replay();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image = window.findChild<ImageView *>("frameOutput");
        QCOMPARE(image->image().pixelColor(1527, 1240), QColor(70, 70, 70, 255));
        auto history = window.findChild<PixelHistoryView *>("pixelHistoryView");
        history->setBackendPath(library, false);
        auto pick = window.findChild<QAction *>("pickPixelHistory");
        QVERIFY(pick->isEnabled());
        pick->setChecked(true);
        done.clear();
        image->pixelSelected(1527, 1240, QColor(70, 70, 70, 255));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        auto records = history->findChild<QTreeWidget *>("historyRecords");
        QCOMPARE(records->topLevelItemCount(), 4);
        QCOMPARE(history->request().at("execution"), nlohmann::json("12675"));
        QCOMPARE(history->request().at("resource_creation"), nlohmann::json("167081"));
        QCOMPARE(history->request().at("resource"), nlohmann::json("4980699392"));
        QCOMPARE(history->request().at("layer"), nlohmann::json(1));
        QCOMPARE(history->request().at("mip"), nlohmann::json(0));
        QVERIFY(history->findChild<QLabel *>("historySummary")->text().contains("Partial history"));
        auto draw = records->topLevelItem(3);
        QCOMPARE(draw->data(0, Qt::UserRole).toULongLong(), 12675ULL);
        QVERIFY(draw->text(5).contains("17980"));
        QVERIFY(draw->text(3).contains("not a fragment"));
        QVERIFY(records->topLevelItem(1)->text(4).contains("unavailable"));
        QVERIFY(records->topLevelItem(1)->text(3).contains("unverified"));
        QTemporaryDir temp;
        history->exportResult(temp.filePath("history.json"));
        QFile exported(temp.filePath("history.json"));
        QVERIFY(exported.open(QIODevice::ReadOnly));
        const auto report = nlohmann::json::parse(exported.readAll().toStdString());
        QCOMPARE(report.at("depth_event_boundaries")[1].at("post").at("raw_unorm16"), nlohmann::json(17980));
        QVERIFY(report.at("partial").get<bool>());
        const auto oldRequest = history->requestId();
        records->setCurrentItem(records->topLevelItem(2));
        window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
        history->findChild<QAction *>("locateHistoryEvent")->trigger();
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 12667ULL);
        QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
        QVERIFY(!history->finish(oldRequest, report));
    }
    void selectedAnalysisCapabilities() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for DX12 analysis capability checks");
        const auto session = openReplaySession(
            std::filesystem::path((root + "/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        ReplaySelection selection;
        selection.session = session->identity();
        QVERIFY(!session->capability(ReplayFeature::ShaderDebug, selection).supported());
        selection.execution = 12675;
        QVERIFY(session->capability(ReplayFeature::ShaderDebug, selection).supported());
        QVERIFY(session->capability(ReplayFeature::Geometry, selection).supported());
        selection.resource = UINT64_MAX; // Invocation analysis does not consume the output readback resource.
        QVERIFY(session->capability(ReplayFeature::ShaderDebug, selection).supported());
        selection.execution = 12667; // Clear has no shader invocation or post-shader mesh.
        QVERIFY(!session->capability(ReplayFeature::ShaderDebug, selection).supported());
        QVERIFY(!session->capability(ReplayFeature::Geometry, selection).supported());
        selection.execution = UINT64_MAX;
        QCOMPARE(session->capability(ReplayFeature::Geometry, selection).status,
                 CapabilityStatus::MissingData);
        selection.session = "stale";
        QCOMPARE(session->capability(ReplayFeature::ShaderDebug, selection).status, CapabilityStatus::Failed);
    }
    void indirectCapabilityGaps() {
        for (bool missing : {true, false}) {
            QTemporaryDir temp;
            const auto path = fixture(temp);
            QByteArray packets;
            uint64_t timestamp{};
            const auto packet = [&](uint64_t type, const QByteArray &body) {
                qword(packets, type);
                qword(packets, ++timestamp);
                qword(packets, 0);
                qword(packets, body.size());
                packets += body;
            };
            if (!missing) {
                QByteArray signature;
                qword(signature, 1);
                signature.append(char(1));
                dword(signature, 16);
                dword(signature, 2);
                signature.append(char(1));
                qword(signature, 2);
                D3D12_INDIRECT_ARGUMENT_DESC argument{};
                argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
                argument.Constant.Num32BitValuesToSet = 1;
                signature.append(reinterpret_cast<const char *>(&argument), sizeof(argument));
                argument = {};
                argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
                signature.append(reinterpret_cast<const char *>(&argument), sizeof(argument));
                dword(signature, 0);
                qword(signature, 99); // Indirect root-argument replay is not implemented.
                const auto iid = __uuidof(ID3D12CommandSignature);
                signature.append(reinterpret_cast<const char *>(&iid), sizeof(iid));
                signature.append(char(1));
                qword(signature, 1);
                qword(signature, 2);
                dword(signature, S_OK);
                packet(0x12793fa7d46faULL, signature);
            }
            QByteArray reset;
            qword(reset, 3);
            qword(reset, 4);
            qword(reset, 0);
            dword(reset, S_OK);
            packet(0x1ae3c721c351aULL, reset);
            QByteArray command;
            qword(command, 3);
            qword(command, 2);
            dword(command, 1);
            qword(command, 5);
            qword(command, 0);
            qword(command, 0);
            qword(command, 0);
            packet(0x1ae3ccddf9f8cULL, command);
            QByteArray close;
            qword(close, 3);
            dword(close, S_OK);
            packet(0x1ae3cd6acf667ULL, close);
            QByteArray submit;
            qword(submit, 6);
            dword(submit, 1);
            submit.append(char(1));
            qword(submit, 1);
            qword(submit, 3);
            packet(0x125482975df48ULL, submit);
            write(path + "/thread-7-0", packets);
            const auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
            ReplaySelection selection;
            selection.session = session->identity();
            selection.execution = 1;
            QCOMPARE(session->capability(ReplayFeature::ShaderDebug, selection).status,
                     missing ? CapabilityStatus::MissingData : CapabilityStatus::NotImplemented);
        }
    }
    void nativePerformanceWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set BF1 capture directory for selected Dispatch performance");
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/bf1_2025_12_10__14_29_55.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("21895");
        QTest::keyClick(locate, Qt::Key_Return);
        auto metrics = window.findChild<RdcCountersView *>("rdcCountersView");
        metrics->setBackendPath("missing-renderdoc.dll", false);
        QVERIFY(!metrics->findChild<QAction *>("rdcCountersBackend")->isEnabled());
        auto read = metrics->findChild<QAction *>("readRdcCounters");
        QVERIFY(read->isEnabled());
        QCOMPARE(metrics->request().at("execution"), nlohmann::json("21895"));
        done.clear();
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        const auto result = metrics->result();
        const auto &measurement = result.at("native_measurement");
        const auto output = qEnvironmentVariable("FLORA_DX12_NATIVE_METRICS");
        if (!output.isEmpty()) metrics->exportResult(output);
        QCOMPARE(measurement.at("execution"), nlohmann::json("21895"));
        QCOMPARE(measurement.at("source_record"), nlohmann::json("274044"));
        QCOMPARE(measurement.at("queue_type"), nlohmann::json(0));
        QCOMPARE(measurement.at("pipeline_statistics_status"), nlohmann::json("measured"));
        QCOMPARE(measurement.at("pipeline_statistics").at("CSInvocations"), nlohmann::json("4096"));
        QCOMPARE(measurement.at("pipeline_statistics").at("VSInvocations"), nlohmann::json("0"));
        const auto begin = std::stoull(measurement.at("timestamp_begin").get<std::string>());
        const auto end = std::stoull(measurement.at("timestamp_end").get<std::string>());
        const auto frequency = std::stoull(measurement.at("timestamp_frequency_hz").get<std::string>());
        QVERIFY(end > begin);
        QVERIFY(frequency > 0);
        QCOMPARE(measurement.at("duration_seconds").get<double>(), double(end - begin) / double(frequency));
        const auto &timeline = measurement.at("timeline");
        QCOMPARE(timeline.at("scope"), nlohmann::json("frame_prefix_submitted_lists"));
        QVERIFY(!timeline.at("entries").empty());
        dx12::StreamDirectory directory(std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        dx12::SubmissionIndex index(directory);
        bool containsSelection{}; size_t calibrated{};
        for (const auto &entry : timeline.at("entries")) {
            const auto count = std::stoull(entry.at("execution_count").get<std::string>());
            const auto submission = std::stoull(entry.at("submission").get<std::string>());
            const auto ordinal = std::stoull(entry.at("list_ordinal").get<std::string>());
            const auto found = std::find_if(index.lists().begin(), index.lists().end(), [&](const auto &list) { return list.submission == submission && list.ordinal == ordinal; });
            QVERIFY(found != index.lists().end()); QVERIFY(found->recording.has_value());
            QCOMPARE(entry.at("queue"), nlohmann::json(std::to_string(found->queue)));
            QCOMPARE(entry.at("generation"), nlohmann::json(std::to_string(index.recordings().at(*found->recording).generation)));
            if (!count) QVERIFY(entry.at("first_execution").is_null());
            const auto first = count ? std::stoull(entry.at("first_execution").get<std::string>()) : 0;
            if (count) QCOMPARE(first, found->firstExecution);
            if (entry.at("clock_scope") == "calibrated_cpu_qpc") ++calibrated;
            if (first <= 21895 && 21895 - first < count) {
                containsSelection = true;
                QVERIFY(std::stoull(entry.at("timestamp_begin").get<std::string>()) <= begin);
                QVERIFY(std::stoull(entry.at("timestamp_end").get<std::string>()) >= end);
            }
        }
        QVERIFY(containsSelection); QVERIFY(calibrated > 0);
        const auto timelineView = metrics->findChild<QTreeWidget *>("rdcReplayTimeline");
        QVERIFY(timelineView->topLevelItemCount() > 0);
        qInfo() << "Measured timeline lists" << timeline.at("entries").size() << "calibrated" << calibrated
                << "queue groups" << timelineView->topLevelItemCount() << "fence observations" << timeline.at("synchronization").size();
        auto rows = metrics->findChild<QTreeWidget *>("rdcCounterValues");
        QCOMPARE(rows->topLevelItemCount(), 12);
        qInfo() << "BF1 native Dispatch seconds" << measurement.at("duration_seconds").get<double>()
                << "frequency" << frequency << "CS invocations" << 4096;
        rows->setCurrentItem(rows->topLevelItem(0));
        window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
        metrics->findChild<QAction *>("locateRdcCounter")->trigger();
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 21895ULL);
        QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
        const auto token = metrics->requestId();
        locate->setText("21622");
        QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(metrics->result().is_null());
        QVERIFY(!metrics->finish(token, result));
    }
    void discardRegionDetails() {
        QByteArray bytes; qword(bytes, 0x1234); qword(bytes, 9007199254740993ULL); bytes.append(char(0));
        const auto decode = [&] { return dx12::decodeArguments(0x1ae3ce6df7dc5ULL,
            format::Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()))); };
        auto result = decode(); QVERIFY(result.supported); QVERIFY(result.values.at("Region").is_null());
        QCOMPARE(result.resources, std::vector<uint64_t>{9007199254740993ULL});
        bytes[16] = char(1); dword(bytes, 1); bytes.append(char(1)); qword(bytes, 1);
        for (const uint32_t n : {2U, 4U, 30U, 40U, 3U, 2U}) dword(bytes, n);
        result = decode(); QCOMPARE(result.values.at("Region").at("FirstSubresource"), nlohmann::json(3));
        QCOMPARE(result.values.at("Region").at("NumSubresources"), nlohmann::json(2));
        QCOMPARE(result.values.at("Region").at("Rects")[0].at("Bottom"), nlohmann::json(40));
        bytes.chop(1); QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode());
        bytes = bytes.left(16); bytes.append(char(1)); dword(bytes, 1); bytes.append(char(0)); dword(bytes, 0); dword(bytes, 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decode());
        bytes = bytes.left(16); bytes.append(char(1)); dword(bytes, 0); bytes.append(char(0)); dword(bytes, 2); dword(bytes, 1);
        result = decode(); QVERIFY(result.values.at("Region").at("Rects").empty());
        QCOMPARE(result.values.at("Region").at("FirstSubresource"), nlohmann::json(2));
    }
    void capturedDiscardHistory() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto path = qEnvironmentVariable("FLORA_DX12_DISCARD_HISTORY");
        if (root.isEmpty() || path.isEmpty()) QSKIP("Set Hellblade2 capture and mip-1 history report");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        auto report = nlohmann::json::parse(file.readAll().toStdString());
        const auto session = openReplaySession(std::filesystem::path((root + "/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString()));
        enrichDx12ResourceHistory(*session, 570673, report);
        size_t discards{}; int last = -1;
        for (size_t i = 0; i < report.at("resource_history").size(); ++i) {
            const auto &row = report.at("resource_history")[i];
            if (row.at("usage_name") != "Discard") continue;
            ++discards; last = int(i);
            QCOMPARE(row.at("content_definition"), nlohmann::json("discarded_until_rewritten"));
            QCOMPARE(row.at("content_change"), nlohmann::json("not_measured"));
            QVERIFY(row.at("captured_discard_region").is_null());
            QCOMPARE(row.at("captured_call").at("arguments").at("UndecodedExtensionBytes"), nlohmann::json(0));
            qInfo() << "Whole-resource discard" << QString::fromStdString(row.at("execution").get<std::string>());
        }
        QCOMPARE(discards, size_t(2)); QVERIFY(last >= 0);
        QVERIFY(!report.at("resource_history")[0].at("captured_resource_barriers").empty());
        applyAppearance(*qApp);
        ResourceHistoryView view; view.resize(1350, 600); view.show();
        view.setContext("hellblade-discard", 36779, 570673, 2149346324304ULL);
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("resourceHistoryRows"); QVERIFY(rows);
        auto item = rows->topLevelItem(last); QVERIFY(item->text(4).contains("discard", Qt::CaseInsensitive));
        rows->setCurrentItem(item); rows->scrollToItem(item);
        QSignalSpy jump(&view, &ResourceHistoryView::eventRequested);
        view.findChild<QAction *>("locateResourceHistoryEvent")->trigger();
        QCOMPARE(jump.size(), 1); QCOMPARE(jump[0][0].toULongLong(), 36710ULL);
        const auto screenshot = qEnvironmentVariable("FLORA_DX12_UI_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(view.grab().save(screenshot));
        const auto output = qEnvironmentVariable("FLORA_DX12_DISCARD_EXPORT");
        if (!output.isEmpty()) view.exportResult(output);
        auto wrong = report; wrong["resource_object"] = "9007199254740993";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, enrichDx12ResourceHistory(*session, 570673, wrong));
        wrong = report; wrong["resource_history"][last]["source_record"] = "1";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, enrichDx12ResourceHistory(*session, 570673, wrong));
    }
    void resourceBarrierDetails() {
        QByteArray bytes;
        qword(bytes, 0x1234);
        dword(bytes, 1);
        bytes.append(char(1));
        qword(bytes, 1);
        dword(bytes, 0);
        dword(bytes, 0);
        qword(bytes, 9007199254740993ULL);
        dword(bytes, UINT32_MAX);
        dword(bytes, 4);
        dword(bytes, 0);
        dword(bytes, 0);
        const auto payload = [&] {
            return format::Bytes(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()));
        };
        const auto decoded = dx12::decodeArguments(0x1ae3c24e5dabeULL, payload());
        QVERIFY(decoded.supported);
        QCOMPARE(decoded.resources, std::vector<uint64_t>{9007199254740993ULL});
        QCOMPARE(decoded.values.at("Barriers")[0].at("Resource"), nlohmann::json("9007199254740993"));
        QCOMPARE(decoded.values.at("Barriers")[0].at("Subresource"), nlohmann::json(UINT32_MAX));
        bytes.chop(1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::decodeArguments(0x1ae3c24e5dabeULL, payload()));
    }
    void backBufferResourceHistory() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto reportPath = qEnvironmentVariable("FLORA_DX12_RESOURCE_HISTORY");
        if (root.isEmpty() || reportPath.isEmpty())
            QSKIP("Set BF1 capture directory and recorded resource history");
        QFile file(reportPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto report = nlohmann::json::parse(file.readAll().toStdString());
        const auto session = openReplaySession(
            std::filesystem::path((root + "/bf1_2025_12_10__14_29_55.gpa_frame").toStdWString()));
        size_t barriers = 0, targets = 0;
        for (auto &row : report.at("resource_history")) {
            QVERIFY(row.at("selectable").get<bool>());
            QCOMPARE(row.at("content_change"), nlohmann::json("not_measured"));
            if (row.at("usage_name") == "ColorTarget") {
                ++targets;
                QCOMPARE(row.at("evidence"), nlohmann::json("bound_use"));
            } else if (row.at("usage_name") == "Barrier") {
                ++barriers;
                const auto detail =
                    session->executionDetails(std::stoull(row.at("execution").get<std::string>()));
                bool found = false;
                for (const auto &barrier : detail.at("arguments").at("Barriers"))
                    if (barrier.value("Resource", "") == "2119382448") {
                        found = true;
                        row["captured_resource_barriers"].push_back(barrier);
                        if (row.at("execution") == "37815") {
                            QCOMPARE(barrier.at("StateBefore"), nlohmann::json(4));
                            QCOMPARE(barrier.at("StateAfter"), nlohmann::json(0));
                            QCOMPARE(barrier.at("Subresource"), nlohmann::json(0));
                        }
                    }
                QVERIFY(found);
            }
        }
        QCOMPARE(barriers, size_t(4));
        QCOMPARE(targets, size_t(56));
        ResourceHistoryView view;
        view.setContext("bf1-resource", 37815, 11399, 2119382448);
        QCOMPARE(view.request().at("resource_creation"), nlohmann::json("11399"));
        QVERIFY(view.finish(view.requestId(), report));
        auto rows = view.findChild<QTreeWidget *>("resourceHistoryRows");
        QCOMPARE(rows->topLevelItemCount(), 60);
        QVERIFY(rows->topLevelItem(0)->text(4).contains("All: 0x0 → 0x4"));
        QVERIFY(rows->topLevelItem(59)->text(4).contains("0: 0x4 → 0x0"));
        rows->setCurrentItem(rows->topLevelItem(1));
        QSignalSpy jump(&view, &ResourceHistoryView::eventRequested);
        view.findChild<QAction *>("locateResourceHistoryEvent")->trigger();
        QCOMPARE(jump.count(), 1);
        QCOMPARE(jump[0][0].toULongLong(), 37273ULL);
        const auto old = view.requestId();
        view.setContext("new-session", 37815, 11399, 2119382448);
        QVERIFY(!view.finish(old, report));
        QCOMPARE(rows->topLevelItemCount(), 0);
        report["resource_creation"] = "11395";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), report));
        view.setContext("wide", 9007199254740993ULL, UINT64_MAX - 1, UINT64_MAX);
        QCOMPARE(view.request().at("execution"), nlohmann::json("9007199254740993"));
        QCOMPARE(view.request().at("resource"), nlohmann::json("18446744073709551615"));
    }
    void bufferResourceHistoryWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        if (root.isEmpty() || library.isEmpty())
            QSKIP("Set BF1 capture directory and RenderDoc");
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/bf1_2025_12_10__14_29_55.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("21622");
        QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        QModelIndex selected;
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            const auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 11815) {
                selected = index;
                break;
            }
        }
        QVERIFY(selected.isValid());
        resources->setCurrentIndex(selected);
        auto history = window.findChild<ResourceHistoryView *>("resourceHistoryView");
        history->setBackendPath(library, false);
        auto read = history->findChild<QAction *>("readResourceHistory");
        QVERIFY(read->isEnabled());
        QCOMPARE(history->request().at("resource"), nlohmann::json("864906160"));
        done.clear();
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        const auto &result = history->result();
        QVERIFY(result.at("gpa_modules_loaded").empty());
        QCOMPARE(result.at("captured_creation").at("id"), nlohmann::json("11815"));
        bool found = false;
        auto rows = history->findChild<QTreeWidget *>("resourceHistoryRows");
        for (size_t i = 0; i < result.at("resource_history").size(); ++i) {
            const auto &row = result.at("resource_history")[i];
            if (row.value("execution", "") != "21622")
                continue;
            found = true;
            QVERIFY(row.at("usage_name").get<std::string>().ends_with("_RWResource"));
            qInfo() << "BF1 root UAV usage"
                    << QString::fromStdString(row.at("usage_name").get<std::string>());
            QCOMPARE(row.at("evidence"), nlohmann::json("bound_use"));
            QCOMPARE(row.at("captured_call").at("source_record"), nlohmann::json("328782"));
            rows->setCurrentItem(rows->topLevelItem(int(i)));
        }
        QVERIFY(found);
        qInfo() << "BF1 buffer resource history uses" << result.at("resource_history").size();
        const auto out = qEnvironmentVariable("FLORA_DX12_BUFFER_HISTORY");
        if (!out.isEmpty())
            history->exportResult(out);
        const auto serial = history->requestId();
        window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
        history->findChild<QAction *>("locateResourceHistoryEvent")->trigger();
        QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
        QCOMPARE(window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                 21622ULL);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 2, 30000);
        QVERIFY(!history->finish(serial, {{"ok", false}, {"error", "stale session"}}));
        QVERIFY(history->result().is_null());
    }
    void backBufferWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set capture directory for BF1 swapchain output");
        const auto path = root + "/bf1_2025_12_10__14_29_55.gpa_frame";
        const auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
        QCOMPARE(session->information().at("swapchain_buffers"), nlohmann::json(3));
        QCOMPARE(session->resource(11399).capturedObject, 0x7e5335b0ULL);
        const auto details = session->resourceDetails(11399).at("swapchain_buffer");
        QCOMPARE(details.at("index"), nlohmann::json(0));
        QCOMPARE(details.at("description").at("Width"), nlohmann::json("1920"));
        QCOMPARE(details.at("description").at("Height"), nlohmann::json(1009));
        ReplaySelection selection;
        selection.session = session->identity();
        selection.execution = 37815;
        selection.resource = 11399;
        QVERIFY(session->capability(ReplayFeature::Replay, selection).supported());
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("37815");
        QTest::keyClick(locate, Qt::Key_Return);
        auto resources = window.findChild<QTableView *>("resources");
        QVERIFY(resources);
        QModelIndex selected;
        for (int row = 0; row < resources->model()->rowCount(); ++row) {
            auto index = resources->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 11399) {
                selected = index;
                break;
            }
        }
        QVERIFY(selected.isValid());
        resources->setCurrentIndex(selected);
        window.findChild<QComboBox *>("outputBoundary")->setCurrentIndex(2);
        const auto replay = window.findChild<QAction *>("replayFrame");
        QVERIFY(replay->isEnabled());
        replay->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        const auto image =
            window.findChild<ImageView *>("frameOutput")->image().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(image.size(), QSize(1920, 1009));
        QVERIFY(window.findChild<QAction *>("exportOutputStorage")->isEnabled());
        const auto output = qEnvironmentVariable("FLORA_DX12_BF1_COLOR");
        if (!output.isEmpty())
            QVERIFY(image.save(output));
        qInfo() << "BF1 first submission color"
                << QCryptographicHash::hash(
                       QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()),
                       QCryptographicHash::Sha256)
                       .toHex();
    }
    void colorHistoryPixelDebugWorkspace() {
        try {
            const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
            const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
            const auto historyPath = qEnvironmentVariable("FLORA_DX12_BF1_COLOR_HISTORY");
            const auto imagePath = qEnvironmentVariable("FLORA_DX12_BF1_COLOR");
            if (root.isEmpty() || library.isEmpty() || historyPath.isEmpty() || imagePath.isEmpty())
                QSKIP("Set BF1 capture, RenderDoc, color history report and native color image");
            QFile input(historyPath);
            QVERIFY(input.open(QIODevice::ReadOnly));
            const auto report = nlohmann::json::parse(input.readAll().toStdString());
            QCOMPARE(report.at("execution"), nlohmann::json("37815"));
            QCOMPARE(report.at("history").size(), size_t(4));
            const auto &first = report.at("history")[0];
            QCOMPARE(first.at("execution"), nlohmann::json("37273"));
            QCOMPARE(first.at("source_record"), nlohmann::json("363202"));
            const QImage image(imagePath);
            QVERIFY(!image.isNull());
            const auto pixel = image.pixelColor(960, 500);
            const int channels[]{pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
            for (size_t c = 0; c < 4; ++c)
                QCOMPARE(
                    int(std::lround(first.at("postMod").at("col").at("floatValue")[c].get<double>() * 255)),
                    channels[c]);
            for (size_t i = 1; i < 4; ++i) {
                const auto &row = report.at("history")[i];
                QVERIFY(row.at("stencilTestFailed").get<bool>());
                QCOMPARE(row.at("preMod").at("col"), row.at("postMod").at("col"));
            }
            MainWindow window;
            QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
            window.openCapture(root + "/bf1_2025_12_10__14_29_55.gpa_frame");
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
            window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
            auto locate = window.findChild<QLineEdit *>("apiLocate");
            locate->setText("37815");
            QTest::keyClick(locate, Qt::Key_Return);
            auto history = window.findChild<PixelHistoryView *>("pixelHistoryView");
            history->setDx12Resource(11399, 2119382448);
            history->selectPixel(2119382448, 960, 500, 0, 0, 0);
            QVERIFY(history->finish(history->requestId(), report));
            const auto oldHistoryRequest = history->requestId();
            auto records = history->findChild<QTreeWidget *>("historyRecords");
            records->setCurrentItem(records->topLevelItem(0));
            auto debug = history->findChild<QAction *>("debugHistoryPixel");
            QVERIFY(debug->isEnabled());
            window.findChild<QLineEdit *>("apiSearch")->setText("filtered-out");
            debug->trigger();
            QVERIFY(window.findChild<QLineEdit *>("apiSearch")->text().isEmpty());
            QCOMPARE(
                window.findChild<QTableView *>("apiLog")->currentIndex().data(Qt::UserRole).toULongLong(),
                37273ULL);
            QVERIFY(!history->finish(oldHistoryRequest, report));
            auto ps = window.findChild<ReplayDebugView *>("replayDebug-ps");
            QCOMPARE(ps->request(), (nlohmann::json{{"action", "debug-pixel"},
                                                    {"execution", "37273"},
                                                    {"x", 960},
                                                    {"y", 500},
                                                    {"sample", 0},
                                                    {"primitive", 0}}));
            ps->setBackendPath(library, false);
            auto read = ps->findChild<QAction *>("replayDebugRead");
            QVERIFY(read->isEnabled());
            done.clear();
            read->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 90000);
            QVERIFY(done.takeLast()[0].toBool());
            QVERIFY(ps->model());
            const auto &result = ps->model()->result();
            QCOMPARE(result.at("execution"), nlohmann::json("37273"));
            QCOMPARE(result.at("pixel_invocation").at("primitive"), nlohmann::json(0));
            QVERIFY(result.at("gpa_modules_loaded").empty());
            QVERIFY(ps->model()->size() > 1);
            ps->findChild<QAction *>("debugStep")->trigger();
            QCOMPARE(ps->model()->position(), int64_t(1));
            qInfo() << "BF1 PS steps" << ps->model()->size() << "RDC event"
                    << result.at("eid").get<unsigned>();
            const auto output = qEnvironmentVariable("FLORA_DX12_PS_REPORT");
            if (!output.isEmpty())
                ps->exportResult(output);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void indirectAnalysisWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        if (root.isEmpty() || library.isEmpty())
            QSKIP("Set capture directory and RenderDoc for the BF1 indirect CS workspace");
        const auto path = root + "/bf1_2025_12_10__14_29_55.gpa_frame";
        const auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
        ReplaySelection selection;
        selection.session = session->identity();
        selection.execution = 35808;
        QCOMPARE(session->executionEvent(35808).workKind, ReplayWorkKind::Dispatch);
        QVERIFY(session->capability(ReplayFeature::ShaderDebug, selection).supported());
        QVERIFY(!session->capability(ReplayFeature::Geometry, selection).supported());
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("35808");
        QTest::keyClick(locate, Qt::Key_Return);
        auto cs = window.findChild<ReplayDebugView *>("replayDebug-cs");
        auto vs = window.findChild<ReplayDebugView *>("replayDebug-vs");
        QVERIFY(cs);
        QVERIFY(vs);
        QVERIFY(!vs->findChild<QAction *>("replayDebugRead")->isEnabled());
        QVERIFY(!window.findChild<ReplayMeshView *>("replayMeshView")
                     ->findChild<QAction *>("replayMeshRead")
                     ->isEnabled());
        cs->setBackendPath(library, false);
        auto read = cs->findChild<QAction *>("replayDebugRead");
        QVERIFY(read->isEnabled());
        QCOMPARE(cs->request().at("execution"), nlohmann::json("35808"));
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(cs->model());
        const auto &result = cs->model()->result();
        QCOMPARE(result.at("execution"), nlohmann::json("35808"));
        QCOMPARE(result.at("backend"), nlohmann::json("dx12"));
        QVERIFY(result.at("gpa_modules_loaded").empty());
        QVERIFY(result.at("disassembly").get<std::string>().find("5297230d-b11ed8d4-8c1b1ca9-2d9bfa5f") !=
                std::string::npos);
        QVERIFY(cs->model()->size() > 1);
        cs->findChild<QAction *>("debugStep")->trigger();
        QCOMPARE(cs->model()->position(), int64_t(1));
        qInfo() << "Indirect CS workspace steps" << cs->model()->size() << "RDC event"
                << result.at("eid").get<unsigned>();
        const auto serial = cs->requestId();
        locate->setText("21622");
        QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(!cs->model());
        QVERIFY(!read->isEnabled());
        QVERIFY(vs->findChild<QAction *>("replayDebugRead")->isEnabled());
        QVERIFY(!cs->finish(serial, {{"ok", false}, {"error", "stale indirect invocation"}}));
    }
    void selectedAnalysisWorkspace() {
        const auto root = qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        const auto library = qEnvironmentVariable("FLORA_RENDERDOC_LIBRARY");
        if (root.isEmpty() || library.isEmpty())
            QSKIP("Set capture directory and RenderDoc library for native DX12 analysis UI");
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), done(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/Talos_2026_10_18__00_55_08.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 30000);
        window.findChild<QComboBox *>("apiEventDomain")->setCurrentIndex(1);
        auto locate = window.findChild<QLineEdit *>("apiLocate");
        locate->setText("12675");
        QTest::keyClick(locate, Qt::Key_Return);
        auto vs = window.findChild<ReplayDebugView *>("replayDebug-vs");
        QVERIFY(vs);
        vs->setBackendPath(library, false);
        auto read = vs->findChild<QAction *>("replayDebugRead");
        QVERIFY(read->isEnabled());
        QCOMPARE(vs->request().at("execution"), nlohmann::json("12675"));
        read->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QVERIFY(vs->model());
        QCOMPARE(vs->model()->size(), size_t(20));
        const auto capture = vs->model()->result().at("capture");
        QVERIFY(vs->model()->files().empty());
        QVERIFY(vs->model()->result().at("frame_sha256").is_null());
        QCOMPARE(vs->model()->result().at("execution"), nlohmann::json("12675"));
        vs->findChild<QAction *>("debugStep")->trigger();
        QCOMPARE(vs->model()->position(), int64_t(1));
        vs->findChild<QSpinBox *>("debugStepIndex")->setValue(19);
        vs->findChild<QLineEdit *>("debugWatchExpression")->setText("SV_Position.y");
        vs->findChild<QAction *>("debugAddWatch")->trigger();
        const auto watch = vs->model()->watchResults();
        QCOMPARE(watch.size(), size_t(1));
        QCOMPARE(watch[0].at("status"), nlohmann::json("available"));
        QVERIFY(std::abs(watch[0].at("values")[0].get<double>() - (-0.3396325707435608)) < 1e-9);
        auto mesh = window.findChild<ReplayMeshView *>("replayMeshView");
        QVERIFY(mesh);
        mesh->setBackendPath(library, false);
        done.clear();
        mesh->findChild<QAction *>("replayMeshRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(mesh->result().at("capture"),
                 capture); // Same exact prefix is reused, without another native replay.
        QCOMPARE(mesh->result().at("vertex_count"), nlohmann::json(8053));
        QCOMPARE(mesh->result().at("index_count"), nlohmann::json(42006));
        QTemporaryDir exported;
        mesh->exportResult(exported.filePath("mesh.obj"));
        QVERIFY(QFileInfo(exported.filePath("mesh.obj")).size() > 1000);
        const auto stale = vs->requestId();
        locate->setText("12679");
        QTest::keyClick(locate, Qt::Key_Return);
        QVERIFY(!vs->model());
        QVERIFY(mesh->result().is_null());
        QVERIFY(!vs->finish(stale, {{"ok", false}, {"error", "stale execution"}}));
        ReplayDebugView wide("vs");
        wide.setContext("wide", 9007199254740993ULL, true);
        QCOMPARE(wide.request().at("execution"), nlohmann::json("9007199254740993"));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            wide.finish(wide.requestId(),
                        {{"ok", true}, {"backend", "dx12"}, {"execution", "9007199254740992"}}));
    }
    void dxilComputeInstructionDebug() {
        const auto path = qEnvironmentVariable("FLORA_DX12_DXIL_CS_TRACE");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_DXIL_CS_TRACE to the Hellblade2 second-submission CS trace");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto result = nlohmann::json::parse(input.readAll().toStdString());
        QCOMPARE(result.at("execution"), nlohmann::json("10135"));
        QCOMPARE(result.at("eid"), nlohmann::json(1814));
        QVERIFY(result.at("disassembly").get<std::string>().find("compiled under SM6.6") !=
                std::string::npos);
        ReplayDebugView view("cs");
        view.setContext("hellblade-dxil-compute", 10135, true);
        QVERIFY(view.finish(view.requestId(), result));
        QCOMPARE(view.model()->size(), size_t(715));
        QVERIFY(view.model()->files().empty());
        view.findChild<QLineEdit *>("debugInstruction")->setText("1677");
        view.findChild<QAction *>("debugToggleInstructionBreakpoint")->trigger();
        view.findChild<QAction *>("debugContinue")->trigger();
        QCOMPARE(result.at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(1677));
        view.findChild<QAction *>("debugStep")->trigger();
        QCOMPARE(result.at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(1678));
        view.model()->addWatch("_1509");
        const auto watch = view.model()->watchResults().at(0);
        QCOMPARE(watch.at("status"), nlohmann::json("available"));
        QCOMPARE(watch.at("values"), nlohmann::json::array({8193}));
        // Raw instruction values must not mask a source declaration that the
        // debugger explicitly reports as undefined at the same name.
        auto shadowed = result;
        shadowed["trace"]["sourceVars"].push_back({{"name", "_1509"}, {"type", 3}, {"rows", 1},
                                                  {"columns", 1}, {"undefinedValue", true},
                                                  {"variables", nlohmann::json::array()}});
        ReplayDebugModel source(std::move(shadowed));
        source.move(size_t(view.model()->position()));
        source.addWatch("_1509");
        QCOMPARE(source.watchResults()[0]["status"], nlohmann::json("unavailable"));
        const auto serial = view.requestId();
        view.findChild<QLineEdit *>("debugThread")->setText("1 0 0");
        QVERIFY(!view.model());
        QVERIFY(!view.finish(serial, result));
    }
    void dxilPixelInstructionDebug() {
        const auto path = qEnvironmentVariable("FLORA_DX12_DXIL_PS_TRACE");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_DXIL_PS_TRACE to the Hellblade2 first Draw trace");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto result = nlohmann::json::parse(input.readAll().toStdString());
        QCOMPARE(result.at("execution"), nlohmann::json("9731"));
        QCOMPARE(result.at("eid"), nlohmann::json(418));
        QVERIFY(result.at("disassembly").get<std::string>().find("compiled under SM6.6") !=
                std::string::npos);
        ReplayDebugView view("ps");
        view.setContext("hellblade-dxil", 9731, true);
        view.selectPixel(169, 269, 0);
        QVERIFY(view.finish(view.requestId(), result));
        QCOMPARE(view.model()->size(), size_t(276));
        // DXIL retains an entry-point name and variable mappings, but its PDB
        // is absent. The source editor must not invent source files.
        QVERIFY(view.model()->files().empty());
        view.findChild<QLineEdit *>("debugInstruction")->setText("273");
        view.findChild<QAction *>("debugToggleInstructionBreakpoint")->trigger();
        view.findChild<QAction *>("debugContinue")->trigger();
        QCOMPARE(result.at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(273));
        view.findChild<QAction *>("debugStep")->trigger();
        view.model()->addWatch("SV_Target");
        const auto watch = view.model()->watchResults().at(0);
        QCOMPARE(watch.at("status"), nlohmann::json("available"));
        QCOMPARE(watch.at("values"),
                 nlohmann::json::array({0.51123046875, -0.82080078125, 428.224609375, 0.41978317499160767}));
        const auto serial = view.requestId();
        view.selectPixel(170, 269, 0);
        QVERIFY(!view.model());
        QVERIFY(!view.finish(serial, result));
    }
    void pixelInstructionDebug() {
        const auto path = qEnvironmentVariable("FLORA_DX12_PS_REPORT");
        const auto historyPath = qEnvironmentVariable("FLORA_DX12_BF1_COLOR_HISTORY");
        if (path.isEmpty() || historyPath.isEmpty())
            QSKIP("Set the recorded BF1 PS trace and color history report");
        QFile input(path), historyInput(historyPath);
        QVERIFY(input.open(QIODevice::ReadOnly));
        QVERIFY(historyInput.open(QIODevice::ReadOnly));
        const auto result = nlohmann::json::parse(input.readAll().toStdString());
        const auto historyResult = nlohmann::json::parse(historyInput.readAll().toStdString());
        ReplayDebugView view("ps");
        view.setContext("bf1-color", 37273, true);
        view.selectPixel(960, 500, 0);
        view.selectPrimitive(0);
        QVERIFY(view.finish(view.requestId(), result));
        QCOMPARE(view.model()->size(), size_t(37));
        QVERIFY(view.model()->files().empty());
        view.findChild<QLineEdit *>("debugInstruction")->setText("34");
        view.findChild<QAction *>("debugToggleInstructionBreakpoint")->trigger();
        view.findChild<QAction *>("debugContinue")->trigger();
        QCOMPARE(result.at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(34));
        view.findChild<QAction *>("debugStep")->trigger();
        view.model()->addWatch("SV_Target");
        const auto watch = view.model()->watchResults().at(0);
        QCOMPARE(watch.at("status"), nlohmann::json("available"));
        QCOMPARE(watch.at("values"),
                 historyResult.at("history")[0].at("shaderOut").at("col").at("floatValue"));
        const auto serial = view.requestId();
        view.selectPrimitive(1);
        QVERIFY(!view.model());
        QVERIFY(!view.finish(serial, result));
        PixelHistoryView history;
        history.setContext("bf1-color-history", 37815, true);
        history.setDx12Resource(11399, 2119382448);
        history.selectPixel(2119382448, 960, 500, 0, 0, 0);
        QVERIFY(history.finish(history.requestId(), historyResult));
        auto records = history.findChild<QTreeWidget *>("historyRecords");
        records->setCurrentItem(records->topLevelItem(1));
        QSignalSpy invocation(&history, &PixelHistoryView::pixelDebugRequested);
        auto action = history.findChild<QAction *>("debugHistoryPixel");
        QVERIFY(action->isEnabled());
        action->trigger();
        QCOMPARE(invocation.count(), 1);
        QCOMPARE(invocation[0][0].toULongLong(), 37309ULL);
        QCOMPARE(invocation[0][4].toUInt(), 1U);
        // Clearing a selected result may emit table selection callbacks during invalidation.
        history.setDx12Resource(11395, 2119326144);
        QVERIFY(!action->isEnabled());
        QCOMPARE(records->topLevelItemCount(), 0);
    }
    void computeInstructionDebug() {
        const auto path = qEnvironmentVariable("FLORA_DX12_CS_TRACE");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_CS_TRACE to the BF1 first Dispatch trace");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto result = nlohmann::json::parse(input.readAll().toStdString());
        ReplayDebugView view("cs");
        view.setContext("bf1-compute", 21895, true);
        QCOMPARE(view.request().at("execution"), nlohmann::json("21895"));
        QCOMPARE(view.request().at("group"), nlohmann::json::array({0, 0, 0}));
        QCOMPARE(view.request().at("thread"), nlohmann::json::array({0, 0, 0}));
        QVERIFY(view.finish(view.requestId(), result));
        QCOMPARE(view.model()->size(), size_t(925));
        QVERIFY(view.model()->files().empty());
        view.model()->addWatch("g_params.g_inputBlocksX");
        view.model()->addWatch("g_params.g_inputBlocksY");
        for (const auto &watch : view.model()->watchResults()) {
            QCOMPARE(watch.at("status"), nlohmann::json("available"));
            QCOMPARE(watch.at("values"), nlohmann::json::array({64}));
        }
        view.findChild<QLineEdit *>("debugInstruction")->setText("155");
        view.findChild<QAction *>("debugToggleInstructionBreakpoint")->trigger();
        view.findChild<QAction *>("debugContinue")->trigger();
        QCOMPARE(result.at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(155));
        view.findChild<QAction *>("debugStep")->trigger();
        QCOMPARE(result.at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(156));
        const auto serial = view.requestId();
        view.findChild<QLineEdit *>("debugThread")->setText("1 2 0");
        QVERIFY(!view.model());
        QVERIFY(!view.finish(serial, result));
        QCOMPARE(view.request().at("thread"), nlohmann::json::array({1, 2, 0}));
    }
    void instructionDebugWithoutSources() {
        const auto path = qEnvironmentVariable("FLORA_DX12_TRACE");
        if (path.isEmpty())
            QSKIP("Set FLORA_DX12_TRACE to the captured Talos VS debug result");
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto result = nlohmann::json::parse(input.readAll().toStdString());
        ReplayDebugView view("vs");
        view.setContext("talos-instructions", 12675, true);
        QVERIFY(view.finish(view.requestId(), result));
        QVERIFY(view.model());
        QVERIFY(view.model()->files().empty());
        view.findChild<QLineEdit *>("debugInstruction")->setText("8");
        view.findChild<QAction *>("debugToggleInstructionBreakpoint")->trigger();
        QCOMPARE(view.model()->instructionBreakpoints(), nlohmann::json::array({8}));
        view.findChild<QAction *>("debugContinue")->trigger();
        QCOMPARE(view.model()->result().at("steps")[size_t(view.model()->position())].at("nextInstruction"),
                 nlohmann::json(8));
        QCOMPARE(view.findChild<QTabWidget *>("debugCodeTabs")->currentIndex(), 0);
        QVERIFY(view.model()->breakpoints().empty());
        const auto config = view.model()->configuration();
        QTemporaryDir temp;
        const auto configPath = temp.filePath("instructions.json").toStdWString();
        writeNativeDebugConfig(configPath, config);
        ReplayDebugModel restored(result);
        restored.importConfiguration(readNativeDebugConfig(configPath));
        QCOMPARE(restored.configuration(), config);
        QCOMPARE(restored.seek(0), size_t(view.model()->position()));
        for (const auto badPoint :
             {nlohmann::json(-1), nlohmann::json(8.5), nlohmann::json("8"), nlohmann::json(UINT64_MAX)}) {
            auto bad = config;
            bad["instruction_breakpoints"] = nlohmann::json::array({badPoint});
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, restored.importConfiguration(bad));
            QCOMPARE(restored.configuration(), config);
        }
        auto duplicate = config;
        duplicate["instruction_breakpoints"].push_back(8);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, restored.importConfiguration(duplicate));
        QCOMPARE(restored.configuration(), config);
        auto legacy = config;
        legacy.erase("instruction_breakpoints");
        restored.importConfiguration(legacy);
        QVERIFY(restored.instructionBreakpoints().empty());
        QCOMPARE(restored.configuration(), legacy);
        view.findChild<QAction *>("debugToggleInstructionBreakpoint")->trigger();
        QVERIFY(view.model()->instructionBreakpoints().empty());
        view.setContext("next-invocation", 12679, true);
        QVERIFY(!view.model());
    }
    void selectedReplayStorageExport() {
        const auto directory = qEnvironmentVariable("FLORA_DX12_REPLAY_OUTPUT");
        if (directory.isEmpty())
            QSKIP("Set FLORA_DX12_REPLAY_OUTPUT to a Talos first-draw CLI replay output");
        const auto result = readWorkerReport(directory, true);
        QCOMPARE(result.replayReport.at("backend").get<std::string>(), std::string("dx12"));
        QCOMPARE(result.replayReport.at("execution").get<std::string>(), std::string("12675"));
        QVERIFY(!result.replayReport.at("frame_complete").get<bool>());
        QCOMPARE(result.replayReport.at("native_texture").at("display_interpretation").get<std::string>(),
                 std::string("unorm16_grayscale_0_to_1"));
        QCOMPARE(result.replayReport.at("device").at("validation_error_count").get<std::string>(),
                 std::string("0"));
        const auto image = readWorkerImage(directory, result.report, true);
        QCOMPARE(image.pixelColor(1527, 1240), QColor(70, 70, 70, 255));
        QTemporaryDir temp;
        exportOutputStorageFiles(directory, temp.filePath("depth.bin"),
                                 result.replayReport.at("output_display"));
        QFile raw(temp.filePath("depth.bin"));
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const auto bytes = raw.readAll();
        QCOMPARE(bytes.size(), qsizetype(8388608));
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                 QByteArray("c463ef68c2774b28af0f8462b3b92f9b56907e64bd42a7c465a29aeee7fea217"));
        uint16_t value{};
        memcpy(&value, bytes.constData() + (1240 * 2048 + 1527) * 2, 2);
        QCOMPARE(value, uint16_t(17980));
        QFile sidecar(temp.filePath("depth.bin.json"));
        QVERIFY(sidecar.open(QIODevice::ReadOnly));
        const auto metadata = nlohmann::json::parse(sidecar.readAll().toStdString());
        QCOMPARE(metadata.at("creation_record").get<std::string>(), std::string("167081"));
        QCOMPARE(metadata.at("subresource").get<unsigned>(), 1U);
    }
    void resourceDataBounds() {
        QTemporaryDir temp;
        const auto path = fixture(temp);
        QByteArray storage(192, 0);
        const uint64_t object = UINT64_MAX - 7, size = 8;
        memcpy(storage.data(), &object, 8);
        memcpy(storage.data() + 40, &size, 8);
        memcpy(storage.data() + 64, "contents", 8);
        const int64_t relative = -128;
        memcpy(storage.data() + 128, &object, 8);
        memcpy(storage.data() + 168, &size, 8);
        memcpy(storage.data() + 176, &relative, 8);
        write(path + "/resources", storage);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            const auto first=directory.resourceData(0),second=directory.resourceData(128);
            QCOMPARE(first.capturedObject,object);QCOMPARE(first.dataOffset,uint64_t(64));
            QCOMPARE(first.bytes.data(),second.bytes.data());
            QCOMPARE(std::string(reinterpret_cast<const char *>(first.bytes.data()),first.bytes.size()),std::string("contents"));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,directory.resourceData(160));
        }
        const uint64_t oversized=UINT64_MAX;
        memcpy(storage.data()+40,&oversized,8);write(path+"/resources",storage);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,directory.resourceData(0));
        }
        const int64_t invalid=INT64_MIN;
        memcpy(storage.data()+176,&invalid,8);write(path+"/resources",storage);
        dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,directory.resourceData(128));
    }
    void repeatedSubmissions() {
        QTemporaryDir temp;const auto path=fixture(temp);
        QByteArray packets;uint64_t timestamp=0;
        const auto packet=[&](uint64_t type,const QByteArray &payload) {
            qword(packets,type);qword(packets,++timestamp);qword(packets,0);qword(packets,payload.size());packets+=payload;
        };
        const uint64_t list=UINT64_MAX-2,queue=UINT64_MAX-3;
        const auto record=[&](uint32_t vertices) {
            QByteArray reset;qword(reset,list);qword(reset,17);qword(reset,0);dword(reset,0);packet(0x1ae3c721c351aULL,reset);
            QByteArray draw;qword(draw,list);dword(draw,vertices);dword(draw,1);dword(draw,0);dword(draw,0);packet(0x1ae3c190d8b1eULL,draw);
            QByteArray close;qword(close,list);dword(close,0);packet(0x1ae3cd6acf667ULL,close);
        };
        const auto submit=[&](std::initializer_list<uint64_t> lists) {
            QByteArray payload;qword(payload,queue);dword(payload,uint32_t(lists.size()));payload.append(char(1));qword(payload,lists.size());
            for(auto object:lists) qword(payload,object);packet(0x125482975df48ULL,payload);
        };
        record(3);submit({list,list});submit({19});record(6);submit({list});
        write(path+"/thread-7-0",packets);
        auto session=openReplaySession(std::filesystem::path(path.toStdWString()));
        const auto first=session->executionDetails(1),second=session->executionDetails(2),third=session->executionDetails(3);
        QCOMPARE(first.at("source_record"),second.at("source_record"));
        QVERIFY(first.at("execution_id")!=second.at("execution_id"));
        QVERIFY(first.at("list_ordinal")!=second.at("list_ordinal"));
        QCOMPARE(first.at("recording_generation").get<std::string>(),std::string("1"));
        QCOMPARE(third.at("recording_generation").get<std::string>(),std::string("2"));
        QVERIFY(first.at("source_record")!=third.at("source_record"));
        QCOMPARE(first.at("queue").get<std::string>(),std::to_string(queue));
        QVERIFY(first.at("recording_complete").get<bool>());
        QCOMPARE(session->submissionDetails().at("lists").at(2).at("status").get<std::string>(),std::string("unresolved_recording"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,session->executionDetails(4));
        // Two submissions of the same recorded draw must not share edits.
        Experiment barriers(*session);
        barriers.setUavBarrierBefore(*session, 2, true);
        QCOMPARE(barriers.dx12Edits(*session).uavBarriersBefore, (std::set<Id>{2}));
        const auto barrierProject = temp.filePath("barriers.json");
        barriers.save(barrierProject);
        Experiment barrierReload(*session); barrierReload.load(barrierProject, *session);
        QVERIFY(barrierReload.undo()); QVERIFY(barrierReload.dx12Edits(*session).uavBarriersBefore.empty());
        QVERIFY(barrierReload.redo());
        QCOMPARE(barrierReload.dx12Edits(*session).uavBarriersBefore, (std::set<Id>{2}));
        barrierReload.setUavBarrierBefore(*session, 2, false);
        QVERIFY(barrierReload.dx12Edits(*session).uavBarriersBefore.empty());
        QVERIFY(barrierReload.undo());
        auto invalidBarrier = barriers.document();
        invalidBarrier["cursor"] = 0; // Validate inactive redo operations too.
        invalidBarrier["history"][0]["operations"][0]["event"] = 2.0;
        write(barrierProject, QByteArray::fromStdString(invalidBarrier.dump()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, barrierReload.load(barrierProject, *session));
        QCOMPARE(barrierReload.dx12Edits(*session).uavBarriersBefore, (std::set<Id>{2}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, barriers.setUavBarrierBefore(*session, 4, true));
        Experiment experiment(*session);
        experiment.setEnabled(*session, 2, false);
        QCOMPARE(experiment.disabledExecutions(*session), (std::set<Id>{2}));
        QVERIFY(experiment.enabled(1)); QVERIFY(!experiment.enabled(2));
        QCOMPARE(experiment.document().at("history").at(0).at("operations").at(0).at("event"), nlohmann::json("2"));
        const auto project = temp.filePath("experiment.json");
        experiment.save(project);
        Experiment restored(*session); restored.load(project, *session);
        QCOMPARE(restored.disabledExecutions(*session), (std::set<Id>{2}));
        QVERIFY(restored.undo()); QVERIFY(restored.disabledExecutions(*session).empty());
        QVERIFY(restored.redo()); QCOMPARE(restored.disabledExecutions(*session), (std::set<Id>{2}));
        QVERIFY(restored.undo()); restored.setEnabled(*session, 3, false);
        QVERIFY(!restored.canRedo()); QCOMPARE(restored.disabledExecutions(*session), (std::set<Id>{3}));
        auto invalid = experiment.document();
        invalid["history"][0]["operations"][0]["event"] = 2.0;
        write(project, QByteArray::fromStdString(invalid.dump()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, restored.load(project, *session));
        QCOMPARE(restored.disabledExecutions(*session), (std::set<Id>{3}));
        invalid = experiment.document(); invalid["cursor"] = 0;
        invalid["history"][0]["operations"][0]["kind"] = "shader";
        write(project, QByteArray::fromStdString(invalid.dump()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, restored.load(project, *session));
        QTemporaryDir otherTemp;
        const auto otherPath = fixture(otherTemp);
        write(otherPath + "/thread-7-0", packets);
        write(otherPath + "/resources", "changed!");
        const auto other = openReplaySession(std::filesystem::path(otherPath.toStdWString()));
        QVERIFY(session->captureIdentity() != other->captureIdentity());
        experiment.save(project);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, restored.load(project, *other));
        dx12::StreamDirectory hashDirectory(std::filesystem::path(otherPath.toStdWString()));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, hashDirectory.contentHash([] { return true; }));
        MainWindow window;QSignalSpy loaded(&window,&MainWindow::captureLoaded);
        window.openCapture(path);QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,10000);
        auto domain=window.findChild<QComboBox *>("apiEventDomain");QVERIFY(domain);domain->setCurrentIndex(1);
        auto api=window.findChild<QTableView *>("apiLog");QVERIFY(api);QCOMPARE(api->model()->rowCount(),3);
        auto filter=window.findChild<QLineEdit *>("apiQueueFilter");QVERIFY(filter);
        filter->setText(QString::number(queue));QCOMPARE(api->model()->rowCount(),3);
        filter->setText("1");QCOMPARE(api->model()->rowCount(),0);
        auto locate=window.findChild<QLineEdit *>("apiLocate");locate->setText("2");QTest::keyClick(locate,Qt::Key_Return);
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(),2ULL);QVERIFY(filter->text().isEmpty());
    }
    void decodedResourceReferences() {
        QTemporaryDir temp;const auto path=fixture(temp);
        QByteArray packets,payload;
        qword(payload,17);qword(payload,UINT64_MAX-1);qword(payload,UINT64_MAX-2);
        qword(packets,0x1ae3c1d190092ULL);qword(packets,1);qword(packets,0);qword(packets,payload.size());packets+=payload;
        write(path+"/thread-7-0",packets);
        CaptureModel model(CaptureModel::Kind::Commands);
        model.setSession(openReplaySession(std::filesystem::path(path.toStdWString())));
        QVERIFY(model.matchesCommand(1,{},UINT64_MAX-1));QVERIFY(model.matchesCommand(1,{},UINT64_MAX-2));
        QVERIFY(!model.matchesCommand(1,{},17));
    }
    void initializationCopyDependencies() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for captured copy dependency checks");
        dx12::StreamDirectory directory(std::filesystem::path((root+"/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        const auto copy=dx12::prepareInitializationCopy(directory,8530,256ULL*1024*1024);
        QCOMPARE(copy.sourceRecord,16ULL);QCOMPARE(copy.destinationRecord,13ULL);
        QCOMPARE(copy.heapRecord,69087ULL);QCOMPARE(copy.count,2130ULL);
        QVERIFY(copy.placed);QCOMPARE(copy.sourceData.size,copy.sourceDesc.Width);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::prepareInitializationCopy(directory,8530,1024));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::prepareInitializationCopy(directory,13,256ULL*1024*1024));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,dx12::prepareInitializationCopy(directory,8530,256ULL*1024*1024,[]{return true;}));
    }
    void swapchainBufferIndex() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for swapchain buffer indexing");
        for(const auto name : {"bf1_2025_12_10__14_29_55.gpa_frame", "Talos_2026_10_18__00_55_08.gpa_frame",
                              "Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame"}) {
            dx12::StreamDirectory directory(std::filesystem::path((root+'/'+name).toStdWString())); dx12::BackBufferIndex index(directory);
            QVERIFY(!index.buffers().empty()); qInfo()<<name<<"backbuffer lifetimes"<<index.buffers().size();
            for(const auto &[record,buffer]:index.buffers()) { QVERIFY(buffer.object); QVERIFY(buffer.description.Width); QVERIFY(buffer.description.Height); }
            if(QString(name).startsWith("Talos")) {
                QCOMPARE(index.buffers().size(),size_t(3)); const auto &buffer=index.buffers().at(35118);
                QCOMPARE(buffer.index,1U); QCOMPARE(buffer.description.Width,1920ULL); QCOMPARE(buffer.description.Height,1080U);
            }
        }
    }
    void nativeDescriptorHandles() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for native descriptor translation");
        dx12::StreamDirectory directory(std::filesystem::path((root+"/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::NativeDevice device(directory); dx12::NativeResources resources(device,directory,256ULL*1024*1024);
        dx12::NativeDescriptors descriptors(device,directory,resources);
        for(size_t i=0;i<directory.packets().size();++i) {
            const auto &p=directory.packets()[i]; if(p.type!=0x15a3edf3ce2bbULL) continue;
            format::Reader r(directory.payload(p)); const auto object=r.read<uint64_t>(); QVERIFY(r.flag()); const auto base=r.read<uint64_t>();
            const auto reference=descriptors.resolve(base,i+1,true); QCOMPARE(reference.heap,object); QCOMPARE(reference.index,0U);
            const auto &creation=directory.packets()[size_t(reference.creation-1)]; const auto recipe=dx12::decodeDescriptorHeap(directory.payload(creation));
            UINT increment{};
            for(const auto &query:directory.packets()) if(query.type==0x127932ffcb166ULL) {
                format::Reader q(directory.payload(query)); const auto owner=q.read<uint64_t>(); const auto type=q.read<UINT>(), stride=q.read<UINT>();
                if(owner==recipe.device && type==UINT(recipe.description.Type)) increment=stride;
            }
            QVERIFY(increment); QVERIFY(recipe.description.NumDescriptors>1);
            const auto translated=descriptors.gpu(base+increment,i+1); auto native=descriptors.heap(object,i+1);
            QCOMPARE(descriptors.resolve(base+increment,i+1,true).index,1U);
            QCOMPARE(translated.ptr,native->GetGPUDescriptorHandleForHeapStart().ptr+device.device()->GetDescriptorHandleIncrementSize(recipe.description.Type));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,descriptors.gpu(base+1,i+1));
            QCOMPARE(resources.information().at("allocation_bytes").get<std::string>(),std::string("0"));
            format::Reader address(directory.payload(directory.packets()[13])); const auto buffer=address.read<uint64_t>(), captured=address.read<uint64_t>();
            const auto nativeAddress=resources.gpuAddress(captured+7,14,16);
            QCOMPARE(nativeAddress,resources.resource(buffer,14)->GetGPUVirtualAddress()+7);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,resources.gpuAddress(captured,14,UINT64_MAX)); return;
        }
        QFAIL("Capture has no shader-visible descriptor heap");
    }
    void descriptorRecipeDecoding() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for descriptor decoding");
        for(const auto name : {"bf1_2025_12_10__14_29_55.gpa_frame", "Talos_2026_10_18__00_55_08.gpa_frame",
                              "Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame"}) {
            dx12::StreamDirectory directory(std::filesystem::path((root+'/'+name).toStdWString()));
            size_t heaps{},writes{},copies{};
            for(size_t i=0;i<directory.packets().size();++i) {
                const auto &p=directory.packets()[i]; const auto bytes=directory.payload(p);
                try {
                    if(p.type==0x127932eb2eb14ULL) {
                        const auto recipe=dx12::decodeDescriptorHeap(bytes); ++heaps;
                        if(SUCCEEDED(recipe.result)) { QVERIFY(recipe.object); QVERIFY(recipe.description.NumDescriptors); }
                        if(heaps==1) QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeDescriptorHeap(bytes.first(bytes.size()-1)));
                    } else if(dx12::isDescriptorWrite(p.type)) {
                        const auto recipe=dx12::decodeDescriptorWrite(p.type,bytes); ++writes; QVERIFY(recipe.destination);
                        if(writes==1) QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeDescriptorWrite(p.type,bytes.first(bytes.size()-1)));
                    } else if(p.type==0x12793300ba04eULL || p.type==0x1279345e6c880ULL) {
                        const auto recipe=dx12::decodeDescriptorCopy(p.type,bytes); ++copies;
                        QCOMPARE(recipe.sources.size(),recipe.sourceSizes.size()); QCOMPARE(recipe.destinations.size(),recipe.destinationSizes.size());
                        if(copies==1) QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeDescriptorCopy(p.type,bytes.first(bytes.size()-1)));
                    }
                } catch(const std::exception &e) { QFAIL(qPrintable(QString("%1 record %2: %3").arg(name).arg(i+1).arg(e.what()))); }
            }
            QVERIFY(heaps>0); QVERIFY(writes>0); qInfo()<<name<<"heaps"<<heaps<<"descriptor writes"<<writes<<"descriptor copies"<<copies;
        }
    }
    void capturedGpuAddressResolution() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for GPU address resolution");
        dx12::StreamDirectory directory(std::filesystem::path((root+"/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::AddressIndex addresses(directory);
        format::Reader r(directory.payload(directory.packets()[13])); const auto object=r.read<uint64_t>(), base=r.read<uint64_t>();
        const auto ref=addresses.resolve(base+7,14,16); QCOMPARE(ref.object,object); QCOMPARE(ref.creation,13ULL); QCOMPARE(ref.offset,7ULL);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,addresses.resolve(base,14,UINT64_MAX));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,addresses.resolve(UINT64_MAX,14));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,addresses.resolve(base,0));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,dx12::AddressIndex(directory,[]{return true;}));
        qInfo()<<"captured address ranges"<<addresses.rangeCount();
    }
    void nativeFrameUploadsAndStates() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for frame upload/state checks");
        dx12::StreamDirectory directory(std::filesystem::path((root+"/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::NativeDevice device(directory); dx12::NativeResources resources(device,directory,256ULL*1024*1024);
        std::vector<uint8_t> expected; uint64_t object{};
        for (const uint64_t id : {185507ULL,185508ULL}) {
            format::Reader r(directory.payload(directory.packets()[size_t(id-1)])); const auto buffer=r.read<uint64_t>();
            if(object) QCOMPARE(buffer,object); object=buffer;
            const auto data=directory.resourceData(r.read<uint64_t>()); format::Reader descriptor(directory.resources().bytes(data.descriptorOffset,64));
            descriptor.skip(32); const auto offset=descriptor.read<uint64_t>();
            if(expected.empty()) { QCOMPARE(offset,0ULL); expected.assign(data.bytes.begin(),data.bytes.end()); }
            else { QCOMPARE(offset,8652ULL); QVERIFY(offset+data.size<=expected.size()); memcpy(expected.data()+offset,data.bytes.data(),size_t(data.size)); }
            resources.restoreMappedData(id);
        }
        auto native=resources.resource(object,185508); void *mapped{}; D3D12_RANGE read{0,expected.size()};
        D3D12_HEAP_PROPERTIES properties{}; D3D12_HEAP_FLAGS heapFlags{};
        QVERIFY(SUCCEEDED(native->GetHeapProperties(&properties,&heapFlags)));
        QVERIFY(!(heapFlags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH));
        QVERIFY(SUCCEEDED(native->Map(0,&read,&mapped))); const bool matches=memcmp(mapped,expected.data(),expected.size())==0;
        D3D12_RANGE none{0,0}; native->Unmap(0,&none); QVERIFY(matches);
        QCOMPARE(resources.information().at("uploaded_bytes").get<std::string>(),std::string("10812"));
        resources.requireState(object,185508,0,D3D12_RESOURCE_STATE_COPY_SOURCE); resources.completeSubmission(D3D12_COMMAND_LIST_TYPE_COPY);
        QCOMPARE(resources.state(object,185508),D3D12_RESOURCE_STATE_GENERIC_READ); // Upload heaps never decay.
        const auto buffer=dx12::decodeResourceRecipe(directory.packets()[12].type,directory.payload(directory.packets()[12])).object;
        resources.requireState(buffer,14,0,D3D12_RESOURCE_STATE_COPY_DEST); resources.completeSubmission(D3D12_COMMAND_LIST_TYPE_COPY);
        QCOMPARE(resources.state(buffer,14),D3D12_RESOURCE_STATE_COMMON);
        resources.requireState(buffer,14,0,D3D12_RESOURCE_STATE_COPY_SOURCE); resources.completeSubmission(D3D12_COMMAND_LIST_TYPE_DIRECT);
        QCOMPARE(resources.state(buffer,14),D3D12_RESOURCE_STATE_COMMON);
        const uint64_t depth=0x128df7100ULL; const auto desc=resources.resource(depth,185550)->GetDesc();
        D3D12_FEATURE_DATA_FORMAT_INFO info{desc.Format}; QVERIFY(SUCCEEDED(device.device()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&info,sizeof(info))));
        const auto count=UINT(desc.MipLevels)*desc.DepthOrArraySize*info.PlaneCount; QVERIFY(count>1);
        for(UINT sub=0;sub<count;++sub) resources.transition(depth,185550,sub,resources.state(depth,185550,sub),D3D12_RESOURCE_STATE_COPY_SOURCE);
        QCOMPARE(resources.state(depth,185550),D3D12_RESOURCE_STATE_COPY_SOURCE); // All individually transitioned subresources converge.
        resources.transition(depth,185550,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,resources.requireState(depth,185550,0,D3D12_RESOURCE_STATE_COPY_SOURCE));
        // The partial-patch buffer above is not write-watched. Exercise a real
        // captured write-watch allocation separately, without replaying a frame.
        dx12::NativeResources watched(device,directory,64ULL*1024*1024);
        bool foundWatch = false;
        for (size_t i=0;i<directory.initializationCount();++i) {
            const auto &packet=directory.packets()[i];
            if (packet.type==0x1279335a5747bULL) {
                const auto recipe=dx12::decodeHeapRecipe(directory.payload(packet));
                if (!(recipe.description.Flags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) || recipe.description.SizeInBytes>64ULL*1024*1024) continue;
                const auto actual=watched.tileHeap(recipe.object,i+1)->GetDesc();
                QCOMPARE(UINT(actual.Flags),UINT(recipe.description.Flags)&~UINT(D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH));
                QCOMPARE(actual.Properties.Type,recipe.description.Properties.Type);
                foundWatch=true; break;
            } else if (dx12::isResourceRecipe(packet.type)) {
                const auto recipe=dx12::decodeResourceRecipe(packet.type,directory.payload(packet));
                if (!(recipe.heapFlags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH) || recipe.description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER || recipe.description.Width>64ULL*1024*1024) continue;
                auto resource=watched.resource(recipe.object,i+1);
                QVERIFY(SUCCEEDED(resource->GetHeapProperties(&properties,&heapFlags)));
                // The runtime adds DENY_* flags for a committed buffer heap.
                QVERIFY(!(heapFlags & D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH));
                QCOMPARE(properties.Type,recipe.properties.Type);
                QCOMPARE(watched.recipe(recipe.object,i+1).heapFlags,recipe.heapFlags);
                foundWatch=true; break;
            }
        }
        QVERIFY(foundWatch);
        QCOMPARE(watched.information().at("write_watch_flags_removed").get<std::string>(),std::string("1"));
    }
    void nativeInitializationGuards() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for native initialization guards");
        dx12::StreamDirectory directory(std::filesystem::path((root+"/Talos_2026_10_18__00_55_08.gpa_frame").toStdWString()));
        dx12::NativeDevice device(directory);
        dx12::NativeReplay limited(device,directory,1024);
        try { limited.initializeTransfers(); QFAIL("Insufficient resource budget was accepted"); }
        catch(const std::runtime_error &e) { QVERIFY(QString::fromUtf8(e.what()).contains("allocation budget exceeded")); }
        QCOMPARE(limited.information().at("resources").at("allocation_bytes").get<std::string>(),std::string("0"));
        QVERIFY(limited.information().at("submissions").empty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,limited.initializeTransfers());
        bool cancel=false;
        dx12::NativeReplay cancelled(device,directory,256ULL*1024*1024,[&]{return cancel;}); cancel=true;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,cancelled.initializeTransfers());
        QVERIFY(cancelled.information().at("submissions").empty());
        QCOMPARE(cancelled.information().at("resources").at("allocation_bytes").get<std::string>(),std::string("0"));
    }
    void tileRecipeDecoding() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for tiled resource decoding");
        dx12::StreamDirectory directory(std::filesystem::path((root+"/Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame").toStdWString()));
        size_t mappings{},copies{};
        for(size_t i=0;i<directory.initializationCount();++i) {
            const auto &p=directory.packets()[i]; const auto bytes=directory.payload(p);
            if(p.type==0x125482264c67fULL) {
                const auto recipe=dx12::decodeTileMapping(bytes); ++mappings;
                QCOMPARE(recipe.regionCount,1U); QCOMPARE(recipe.rangeCount,1U);
                QCOMPARE(recipe.starts.size(),size_t(1)); QCOMPARE(recipe.sizes.size(),size_t(1));
                QCOMPARE(recipe.rangeFlags.size(),size_t(1)); QCOMPARE(recipe.heapOffsets.size(),size_t(1)); QCOMPARE(recipe.tileCounts.size(),size_t(1));
                QCOMPARE(recipe.tileCounts[0],1U);
                if(mappings==1) {
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeTileMapping(bytes.first(bytes.size()-1)));
                    auto bad=std::vector<uint8_t>(bytes.begin(),bytes.end()); const uint64_t invalid=UINT64_MAX; memcpy(bad.data()+21,&invalid,8);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeTileMapping(bad));
                }
            } else if(p.type==0x1ae3cba2cc519ULL) {
                const auto recipe=dx12::decodeTileCopy(bytes); ++copies;
                QCOMPARE(recipe.size.NumTiles,4096U); QCOMPARE(recipe.flags,D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeTileCopy(bytes.first(bytes.size()-1)));
            }
        }
        QCOMPARE(mappings,size_t(8192)); QCOMPARE(copies,size_t(2));
    }
    void resourceRecipeDecoding() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for captured resource decoding");
        for(const auto name : {"bf1_2025_12_10__14_29_55.gpa_frame", "Talos_2026_10_18__00_55_08.gpa_frame",
                              "Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame"}) {
            dx12::StreamDirectory directory(std::filesystem::path((root+'/'+name).toStdWString()));
            size_t resources{},heaps{},modern{};
            for(size_t i=0;i<directory.packets().size();++i) {
                const auto &p=directory.packets()[i]; const auto bytes=directory.payload(p);
                try {
                    if(dx12::isResourceRecipe(p.type)) {
                        const auto recipe=dx12::decodeResourceRecipe(p.type,bytes); ++resources;
                        if(SUCCEEDED(recipe.result)) { QVERIFY(recipe.object); QVERIFY(recipe.description.Width); }
                        if(recipe.kind==dx12::ResourceRecipe::Kind::Committed3) ++modern;
                        if(resources==1) QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeResourceRecipe(p.type,bytes.first(bytes.size()-1)));
                    } else if(p.type==0x1279335a5747bULL) {
                        const auto recipe=dx12::decodeHeapRecipe(bytes); ++heaps;
                        if(SUCCEEDED(recipe.result)) { QVERIFY(recipe.object); QVERIFY(recipe.description.SizeInBytes); }
                        if(heaps==1) QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodeHeapRecipe(bytes.first(bytes.size()-1)));
                    }
                } catch(const std::exception &e) { QFAIL(qPrintable(QString("%1 record %2: %3").arg(name).arg(i+1).arg(e.what()))); }
            }
            QVERIFY(resources>0); qInfo()<<name<<"resources"<<resources<<"heaps"<<heaps<<"modern"<<modern;
        }
    }
    void pipelineRecipeDecoding() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for captured pipeline decoding");
        for(const auto name : {"bf1_2025_12_10__14_29_55.gpa_frame", "Talos_2026_10_18__00_55_08.gpa_frame",
                              "Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame"}) {
            dx12::StreamDirectory directory(std::filesystem::path((root+'/'+name).toStdWString()));
            size_t roots{},pipelines{},streams{},mesh{},failed{};
            for(size_t i=0;i<directory.packets().size();++i) {
                const auto &p=directory.packets()[i];const auto bytes=directory.payload(p);
                try {
                    if(p.type==0x127938051b445ULL) {
                        auto recipe=dx12::decodeRootSignature(bytes);++roots;
                        if(SUCCEEDED(recipe.result)) QVERIFY(!recipe.bytecode.empty());
                    } else if(dx12::isPipelineRecipe(p.type)) {
                        auto recipe=dx12::decodePipelineRecipe(p.type,bytes);++pipelines;
                        if(FAILED(recipe.result)) ++failed;
                        if(recipe.kind==dx12::PipelineRecipe::Kind::Stream) ++streams;
                        for(const auto &[stage,shader]:recipe.shaders) if(stage=="MS") ++mesh;
                        if(pipelines==1) {
                            qInfo()<<name<<"first pipeline"<<i+1<<QString::fromStdString(dx12::describePipelineRecipe(recipe).dump());
                            QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodePipelineRecipe(p.type,bytes.first(bytes.size()-1)));
                            if(recipe.kind==dx12::PipelineRecipe::Kind::Stream) {
                                auto malformed=std::vector<uint8_t>(bytes.begin(),bytes.end());
                                const uint64_t invalidSize=UINT64_MAX;memcpy(malformed.data()+9,&invalidSize,8);
                                QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodePipelineRecipe(p.type,malformed));
                                malformed.assign(bytes.begin(),bytes.end());
                                const uint32_t invalidType=UINT32_MAX;memcpy(malformed.data()+26,&invalidType,4);
                                QVERIFY_THROWS_EXCEPTION(std::runtime_error,dx12::decodePipelineRecipe(p.type,malformed));
                            }
                        }
                    }
                } catch(const std::exception &e) { QFAIL(qPrintable(QString("%1 record %2: %3").arg(name).arg(i+1).arg(e.what()))); }
            }
            qInfo()<<name<<"roots"<<roots<<"pipelines"<<pipelines<<"stream"<<streams<<"mesh"<<mesh<<"captured failures"<<failed;
            QVERIFY(roots>0);QVERIFY(pipelines>0);
            if(QString(name).startsWith("Hellblade2")) { QCOMPARE(streams,size_t(11555));QCOMPARE(mesh,size_t(72)); }
        }
    }
    void originalDirectories() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("Set FLORA_DX12_CAPTURE_DIR for real-directory indexing");
        const std::array<std::pair<const char *,size_t>,3> cases{{
            {"bf1_2025_12_10__14_29_55.gpa_frame",363164},
            {"Talos_2026_10_18__00_55_08.gpa_frame",185466},
            {"Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame",577511}}};
        for(const auto &[name,count]:cases){
            dx12::StreamDirectory directory(std::filesystem::path((root+'/'+name).toStdWString()));
            QCOMPARE(directory.initializationCount(),count);
            QVERIFY(directory.packets().size()>count);
            qInfo()<<name<<"initialization"<<count<<"frame"<<directory.packets().size()-count;
            size_t decoded{}, creations{}, extensions{},copies{},above4G{};
            for(const auto &p: directory.packets()) {
                const auto args=dx12::decodeArguments(p.type,directory.payload(p));
                if(args.supported) { ++decoded; if(args.consumed!=p.size) ++extensions; }
                if(args.createdResource) ++creations;
                if(p.type==0x20000000000000ULL) {
                    format::Reader reader(directory.payload(p));reader.skip(8);
                    const auto data=directory.resourceData(reader.read<uint64_t>());
                    QCOMPARE(data.kind,0U);QCOMPARE(data.bytes.size(),size_t(data.size));
                    ++copies;if(data.dataOffset>UINT32_MAX) ++above4G;
                }
            }
            QVERIFY(creations>0);
            qInfo()<<"decoded arguments"<<decoded<<"resource creations"<<creations<<"extensions"<<extensions;
            qInfo()<<"resource copy ranges"<<copies<<"above 4 GiB"<<above4G;
            if(QString(name).startsWith("Hellblade2")) QCOMPARE(above4G,size_t(1473));
            dx12::SubmissionIndex submissions(directory);
            size_t unresolved{};for(const auto &list:submissions.lists()) if(!list.recording) ++unresolved;
            qInfo()<<"submitted lists"<<submissions.lists().size()<<"unresolved"<<unresolved<<"execution instances"<<submissions.executionCount();
            if(submissions.executionCount()) {
                const auto first=submissions.execution(1),last=submissions.execution(submissions.executionCount());
                QVERIFY(first.record>0);QVERIFY(last.record<=directory.packets().size());
            }
        }
    }
    void dx11ReplayAndSessionSwitch() {
        const auto root=qEnvironmentVariable("FLORA_DX12_CAPTURE_DIR");
        if(root.isEmpty()) QSKIP("External DX11 comparison capture is not configured");
        QTemporaryDir temp;
        MainWindow window;
        QSignalSpy done(&window,&MainWindow::taskFinished), loaded(&window,&MainWindow::captureLoaded);
        window.openCapture(root+"/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),60000);
        QVERIFY(done.takeLast()[0].toBool());
        auto image=window.findChild<ImageView *>("frameOutput");QVERIFY(image);
        auto pixels=image->image().convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(pixels.size(),QSize(2560,1440));
        QByteArray bytes(reinterpret_cast<const char *>(pixels.constBits()),pixels.sizeInBytes());
        QCOMPARE(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex(),
                 QByteArray("2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"));
        auto api=window.findChild<QTableView *>("apiLog");QVERIFY(api);
        api->setCurrentIndex(api->model()->index(2,0));
        auto geometry=window.findChild<QAction *>("inspectGeometry");QVERIFY(geometry);
        QVERIFY(geometry->isEnabled());geometry->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(),30000);
        QVERIFY(done.takeLast()[0].toBool());
        for (const auto name : {"Talos_2026_10_18__00_55_08.gpa_frame", "bf1_2025_12_10__14_29_55.gpa_frame",
                               "Hellblade2-Win64-Shipping_2026_10_17__03_44_05.gpa_frame"}) {
            loaded.clear(); window.openCapture(root+'/'+name);
            QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
            QVERIFY(!window.busy());QVERIFY(image->image().isNull());
            auto proxy=qobject_cast<QSortFilterProxyModel *>(api->model());QVERIFY(proxy);
            QVERIFY(proxy->sourceModel()->rowCount()>100000);
            auto resource=window.findChild<QTableView *>("resources");QVERIFY(resource);
            QVERIFY(resource->model()->rowCount()>0);
            resource->setCurrentIndex(resource->model()->index(0,0));
            api->setCurrentIndex(api->model()->index(api->model()->rowCount()-1,0));
            QVERIFY(!geometry->isEnabled());
        }
        // Start a real worker and replace its session before it can publish.
        loaded.clear();window.openCapture(root+"/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        const auto path=fixture(temp);loaded.clear();window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,30000);
        QCOMPARE(api->model()->rowCount(),2);QVERIFY(image->image().isNull());QVERIFY(!window.busy());
    }
};
QTEST_MAIN(Dx12SessionTests)
#include "Dx12SessionTests.moc"
