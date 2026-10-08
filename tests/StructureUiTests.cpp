#include "StructureCapture.h"
#include "app/CaptureStructureDialog.h"
#include "app/Appearance.h"
#include "application/ContextInspector.h"
#include <QFileDialog>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QThreadPool>
#include <QTreeView>
#include <QtTest>
using namespace flora;
namespace {
QByteArray read(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Read failed");return file.readAll();}
void save(const QString &path,const QByteArray &bytes){QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())throw std::runtime_error("Write failed");}
void choose(CaptureStructureDialog &dialog,const QString &path,bool switchTab=false) {
    bool seen=false;QTimer poll,watchdog;watchdog.setSingleShot(true);
    QObject::connect(&poll,&QTimer::timeout,&poll,[&]{if(auto chooser=dialog.findChild<QFileDialog *>()){seen=true;if(switchTab)dialog.findChild<QTabWidget *>()->setCurrentIndex(0);auto edit=chooser->findChild<QLineEdit *>("fileNameEdit");QVERIFY(edit);edit->setText(path);QMetaObject::invokeMethod(chooser,"accept",Qt::QueuedConnection);}});
    QObject::connect(&watchdog,&QTimer::timeout,&watchdog,[&]{if(auto chooser=dialog.findChild<QFileDialog *>())chooser->reject();});
    poll.start(10);watchdog.start(30000);dialog.findChild<QPushButton *>("exportCaptureStructure")->click();QVERIFY(seen);
}
}
class StructureUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase(){QCoreApplication::setOrganizationName("FloraGPA-StructureTests");QCoreApplication::setApplicationName("FloraGPA-StructureTests");QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);applyAppearance(*qApp);}
    void loading_data(){QTest::addColumn<QString>("mode");for(auto mode:{"success","cancel","late-cancel","close","destroy"})QTest::newRow(mode)<<QString(mode);}
    void loading() {
        QFETCH(QString,mode);QTemporaryDir root;const auto path=root.filePath("structure.gpa_frame");testing::structureCapture(100000).save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString());std::weak_ptr<Frame> lifetime=frame;
        auto owner=std::make_unique<CaptureStructureDialog>(frame);frame.reset();auto &dialog=*owner;QSignalSpy finished(&dialog,&CaptureStructureDialog::inspectionFinished);
        int ticks=0;QTimer pulse;connect(&pulse,&QTimer::timeout,&pulse,[&]{if(owner&&owner->busy())++ticks;});pulse.start(1);dialog.show();
        QTimer::singleShot(0,&dialog,[&]{
            if(mode=="late-cancel") {auto loader=dialog.findChild<QFutureWatcherBase *>("structureLoader");QVERIFY(loader);loader->waitForFinished();dialog.findChild<QPushButton *>("cancelCaptureStructure")->click();}
            else if(mode=="cancel")dialog.findChild<QPushButton *>("cancelCaptureStructure")->click();
            else if(mode=="close")dialog.reject();
            else if(mode=="destroy")owner.reset();
        });
        if(mode=="destroy") {QTRY_VERIFY_WITH_TIMEOUT(!owner,30000);QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(),0,30000);QVERIFY(lifetime.expired());return;}
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,30000);QVERIFY(!dialog.busy());QCOMPARE(finished.last()[0].toBool(),mode=="success");
        if(mode=="success") {QVERIFY(ticks>0);auto view=dialog.findChild<QTreeView *>("commandListInventory");QVERIFY(view&&view->model());qInfo()<<"Structure loading ticks:"<<ticks;}
        else if(mode!="close") {finished.clear();dialog.findChild<QPushButton *>("retryCaptureStructure")->click();QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,30000);QVERIFY(finished.last()[0].toBool());}
        owner.reset();QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(),0,30000);QVERIFY(lifetime.expired());
        QFile released(path);QVERIFY(released.open(QIODevice::ReadWrite));
    }
    void exporting_data(){QTest::addColumn<QString>("mode");for(auto mode:{"success","tab-switch","cancel","close","destroy","locked"})QTest::newRow(mode)<<QString(mode);}
    void exporting() {
        QFETCH(QString,mode);QTemporaryDir root;const auto path=root.filePath("structure.gpa_frame");testing::structureCapture(100000).save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString());const auto expected=QByteArray::fromStdString(inspectCommandLists(*frame).dump(2)+"\n");
        auto owner=std::make_unique<CaptureStructureDialog>(frame);auto &dialog=*owner;QSignalSpy loaded(&dialog,&CaptureStructureDialog::inspectionFinished),exported(&dialog,&CaptureStructureDialog::exportFinished);dialog.show();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(),1,30000);QVERIFY(loaded.last()[0].toBool());dialog.findChild<QTabWidget *>()->setCurrentIndex(1);
        const auto target=root.filePath("capture.json");save(target,"Old document");
        HANDLE lock=INVALID_HANDLE_VALUE;if(mode=="locked"){lock=CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);QVERIFY(lock!=INVALID_HANDLE_VALUE);}
        bool acted=false;int ticks=0;QTimer poll;connect(&poll,&QTimer::timeout,&poll,[&]{
            if(!owner||!dialog.findChild<QFutureWatcherBase *>("structureExporter"))return;++ticks;
            if(acted)return;acted=true;
            if(mode=="cancel")dialog.findChild<QPushButton *>("cancelCaptureStructure")->click();
            if(mode=="close")dialog.reject();
            if(mode=="destroy")owner.reset();
        });poll.start(1);
        // Keep a real existing destination through Qt's overwrite confirmation.
        bool confirmed=false;QTimer confirmation;
        connect(&confirmation,&QTimer::timeout,&confirmation,[&]{if(!owner)return;for(auto box:dialog.findChildren<QMessageBox *>()){confirmed=true;box->button(QMessageBox::Yes)->click();}});confirmation.start(10);
        choose(dialog,target,mode=="tab-switch");
        if(mode=="destroy"){QTRY_VERIFY_WITH_TIMEOUT(!owner,30000);QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(),0,30000);QCOMPARE(read(target),QByteArray("Old document"));return;}
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(),1,30000);poll.stop();confirmation.stop();QVERIFY(acted);QVERIFY(confirmed);
        const bool success=mode=="success"||mode=="tab-switch";QCOMPARE(exported.last()[0].toBool(),success);QCOMPARE(read(target),success?expected:QByteArray("Old document"));QVERIFY(ticks>0);
        if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
        if(mode!="close") {
            dialog.findChild<QTabWidget *>()->setCurrentIndex(1);exported.clear();
            confirmation.start(10);choose(dialog,target);QTRY_COMPARE_WITH_TIMEOUT(exported.size(),1,30000);confirmation.stop();QVERIFY(exported.last()[0].toBool());QCOMPARE(read(target),expected);
        }
    }
    void original_data() {
        QTest::addColumn<QString>("name");
        for(auto name:{"GF2_Exilium_2026_03_03__00_19_35.gpa_frame","bf1_2026_01_21__16_53_05.gpa_frame","helldivers2_2026_04_02__18_02_58.gpa_frame","analysis/capture_samples/legacy/deferred1-each0-restore0-repeat2.gpa_frame","analysis/capture_samples/legacy/deferred1-each1-restore1-repeat2.gpa_frame"})QTest::newRow(name)<<QString(name);
    }
    void original() {
        QFETCH(QString,name);const auto root=qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");if(root.isEmpty())QSKIP("Set FLORA_TEST_CAPTURE_DIR for original structure inspection");
        auto frame=std::make_shared<Frame>((root+'/'+name).toStdWString());
        const auto contexts=inspectContexts(*frame),lists=inspectCommandLists(*frame);CaptureStructureDialog dialog(frame);dialog.show();
        QSignalSpy loaded(&dialog,&CaptureStructureDialog::inspectionFinished),exported(&dialog,&CaptureStructureDialog::exportFinished);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(),1,30000);QVERIFY(loaded.last()[0].toBool());QTemporaryDir destination;
        for(int tab=0;tab<2;++tab){
            dialog.findChild<QTabWidget *>()->setCurrentIndex(tab);const auto path=destination.filePath(QString::number(tab)+".json");exported.clear();choose(dialog,path);
            QTRY_COMPARE_WITH_TIMEOUT(exported.size(),1,30000);QVERIFY(exported.last()[0].toBool());
            QCOMPARE(read(path),QByteArray::fromStdString((tab?lists:contexts).dump(2)+"\n"));
        }
    }
    void malformedList() {
        QTemporaryDir root;auto capture=testing::structureCapture(1);capture.add(201,5,0x9a,{0});const auto path=root.filePath("bad-list.gpa_frame");capture.save(path);
        CaptureStructureDialog dialog(std::make_shared<Frame>(path.toStdWString()));dialog.show();QSignalSpy loaded(&dialog,&CaptureStructureDialog::inspectionFinished),exported(&dialog,&CaptureStructureDialog::exportFinished);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(),1,30000);QVERIFY(loaded.last()[0].toBool());dialog.findChild<QTabWidget *>()->setCurrentIndex(1);
        const auto target=root.filePath("error.json");choose(dialog,target);QTRY_COMPARE_WITH_TIMEOUT(exported.size(),1,30000);QVERIFY(exported.last()[0].toBool());
        const auto error=nlohmann::json::parse(read(target).toStdString());QCOMPARE(error,nlohmann::json({{"error","Command-list resource requires a 16-byte payload"}}));
    }
};
QTEST_MAIN(StructureUiTests)
#include "StructureUiTests.moc"
