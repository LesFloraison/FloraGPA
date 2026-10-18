#include "application/ReplaySession.h"
#include "application/Experiment.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
#include "dx12/SubmissionIndex.h"
#include "dx12/QueryReplayIndex.h"
#include "dx12/InitializationCopy.h"
#include "dx12/PipelineRecipes.h"
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
        const auto resolve = [&](uint32_t count, bool truncate = false) {
            QByteArray p; qword(p, list); qword(p, heap); dword(p, 0); dword(p, 0); dword(p, count); qword(p, 42); qword(p, 0);
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
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, dx12::QueryReplayIndex(directory, submissions, [] { return true; }));
        const auto session = openReplaySession(std::filesystem::path(path.toStdWString()));
        ReplaySelection selection; selection.execution = 3; selection.before = true;
        QVERIFY(session->capability(ReplayFeature::Replay, selection).supported());
        QCOMPARE(session->capability(ReplayFeature::Performance, selection).status, CapabilityStatus::MissingData);
        selection.before = false;
        QCOMPARE(session->capability(ReplayFeature::Replay, selection).status, CapabilityStatus::MissingData);
        QCOMPARE(session->executionDetails(3).at("query_replay_coverage"), queries.details(3));
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
