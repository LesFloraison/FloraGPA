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
#include "app/RdcCountersView.h"
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
        const auto waits = dx12::inspectObservationWaits(directory, draw.submission);
        QCOMPARE(waits.size(), size_t(2));
        QCOMPARE(waits[0].record, 577992ULL);
        QCOMPARE(waits[1].record, 578048ULL);
        QCOMPARE(waits[0].handle, 3964ULL);
        try {
            (void)dx12::inspectObservationWaits(directory, 579109);
            QFAIL("Later CPU event registration must require synchronization reconstruction");
        } catch (const std::runtime_error &e) {
            QVERIFY(QString::fromUtf8(e.what()).contains("578875"));
        }
    }
    void observationWaitBoundaries() {
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
            const auto waits = dx12::inspectObservationWaits(directory, 4);
            QCOMPARE(waits.size(), size_t(1));
            QCOMPARE(waits.front().record, 1ULL);
            QCOMPARE(waits.front().handle, UINT64_MAX);
            // The two Draws belong to the other thread, so their GPU prefix
            // has no dependency on this polling thread's unknown CPU event.
            QCOMPARE(directory.files().at(waits.front().file).thread, 8ULL);
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     dx12::inspectObservationWaits(directory, 4, [] { return true; }));
        }
        QByteArray draw;
        qword(draw, 7);
        for (int i = 0; i < 4; ++i)
            dword(draw, 1);
        packet(0x1ae3c190d8b1eULL, 40, draw);
        write(path + "/thread-8-1", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            QCOMPARE(dx12::inspectObservationWaits(directory, 4).size(), size_t(1));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::inspectObservationWaits(directory, 5));
        }
        // A second stream file for the same captured thread must not bypass
        // its wait when the directory merges records by CPU timestamp.
        thread.clear();
        packet(0x27000000000000ULL, 5, wait);
        write(path + "/thread-8-1", thread);
        thread.clear();
        packet(0x1ae3c190d8b1eULL, 40, draw);
        write(path + "/thread-8-2", thread);
        {
            dx12::StreamDirectory directory(std::filesystem::path(path.toStdWString()));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::inspectObservationWaits(directory, 4));
        }
        thread.clear();
        packet(0x27000000000000ULL, 5, wait.chopped(1));
        write(path + "/thread-8-1", thread);
        dx12::StreamDirectory truncated(std::filesystem::path(path.toStdWString()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, dx12::inspectObservationWaits(truncated, 4));
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
        auto rows = metrics->findChild<QTreeWidget *>("rdcCounterValues");
        QCOMPARE(rows->topLevelItemCount(), 12);
        qInfo() << "BF1 native Dispatch seconds" << measurement.at("duration_seconds").get<double>()
                << "frequency" << frequency << "CS invocations" << 4096;
        const auto output = qEnvironmentVariable("FLORA_DX12_NATIVE_METRICS");
        if (!output.isEmpty())
            metrics->exportResult(output);
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
