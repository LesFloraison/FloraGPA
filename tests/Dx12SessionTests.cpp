#include "application/ReplaySession.h"
#include "dx12/StreamDirectory.h"
#include "dx12/StreamArguments.h"
#include "app/MainWindow.h"
#include "app/Models.h"
#include "app/Views.h"
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

using namespace flora;
namespace {
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot create test input");
}
void qword(QByteArray &bytes, uint64_t value) { bytes.append(reinterpret_cast<const char *>(&value), 8); }
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
            size_t decoded{}, creations{}, extensions{};
            for(const auto &p: directory.packets()) {
                const auto args=dx12::decodeArguments(p.type,directory.payload(p));
                if(args.supported) { ++decoded; if(args.consumed!=p.size) ++extensions; }
                if(args.createdResource) ++creations;
            }
            QVERIFY(creations>0);
            qInfo()<<"decoded arguments"<<decoded<<"resource creations"<<creations<<"extensions"<<extensions;
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
