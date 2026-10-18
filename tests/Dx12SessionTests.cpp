#include "application/ReplaySession.h"
#include "application/Experiment.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
#include "dx12/SubmissionIndex.h"
#include "dx12/InitializationCopy.h"
#include "dx12/PipelineRecipes.h"
#include "dx12/NativeResources.h"
#include "dx12/TileRecipes.h"
#include "dx12/NativeReplay.h"
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
#include "application/NativeDebugConfig.h"
#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <d3dcompiler.h>

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
