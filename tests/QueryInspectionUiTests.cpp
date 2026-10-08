#include "QueryInspectionCapture.h"
#include "app/MainWindow.h"
#include "app/Appearance.h"
#include <QSignalSpy>
#include <QStatusBar>
#include <QThreadPool>
#include <QtTest>
using namespace flora;
namespace {
QAction *cancelAction(MainWindow &window) {
    for(auto action:window.findChildren<QAction *>())if(action->text()=="Cancel")return action;
    return nullptr;
}
CaptureModel *model(MainWindow &window) {
    auto proxy=static_cast<CaptureFilter *>(window.findChild<QTableView *>("apiLog")->model());
    return static_cast<CaptureModel *>(proxy->sourceModel());
}
}
class QueryInspectionUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA-QueryInspectionTests");
        QCoreApplication::setApplicationName("FloraGPA-QueryInspectionTests");applyAppearance(*qApp);
    }
    void lifecycle_data() { QTest::addColumn<QString>("mode");for(const auto name:{"success","cancel","late-cancel","switch","close","destroy"})QTest::newRow(name)<<QString(name); }
    void lifecycle() {
        QFETCH(QString,mode); QTemporaryDir root;
        const auto baseline=root.filePath("baseline.gpa_frame"),large=root.filePath("large.gpa_frame");
        testing::queryInspectionCapture(1,2).save(baseline);testing::queryInspectionCapture().save(large);
        auto owner=std::make_unique<MainWindow>(); auto &window=*owner;window.show();
        QSignalSpy done(&window,&MainWindow::taskFinished),loaded(&window,&MainWindow::captureLoaded);
        window.openCapture(baseline);QTRY_VERIFY_WITH_TIMEOUT(!done.empty()&&!window.busy(),30000);QVERIFY(done.last()[0].toBool());
        const auto expected=model(window)->command(1000);QCOMPARE(expected["query_result"]["query_type"],nlohmann::json(2));
        done.clear();loaded.clear();int ticks=0;bool phase=false,acted=false;QTimer pulse;
        connect(&pulse,&QTimer::timeout,&pulse,[&]{if(owner&&owner->busy())++ticks;});
        const auto connection=connect(window.statusBar(),&QStatusBar::messageChanged,&window,[&](const QString &text){
            if(phase||text!="Reading Query history…")return;
            phase=true;pulse.start(1);
            if(mode=="late-cancel") {
                // Finish the producer without delivering its queued completion.
                // This deliberately blocks only the test's event thread.
                auto loader=window.findChild<QFutureWatcherBase *>("captureLoader");QVERIFY(loader);
                loader->waitForFinished();QVERIFY(loader->isFinished());acted=true;
                auto action=cancelAction(window);QVERIFY(action&&action->isEnabled());action->trigger();return;
            }
            QTimer::singleShot(0,&window,[&]{
                acted=true;
                if(mode=="destroy")owner.reset();
                else if(mode=="close")window.close();
                else if(mode!="success") {auto action=cancelAction(window);QVERIFY(action&&action->isEnabled());action->trigger();}
            });
        });
        window.openCapture(large);
        if(mode=="destroy") {
            QTRY_VERIFY_WITH_TIMEOUT(!owner,30000);QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(),0,30000);
            QVERIFY(phase&&acted);QCOMPARE(loaded.size(),0);return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty()&&!window.busy(),60000);disconnect(connection);pulse.stop();QVERIFY(phase&&acted);
        if(mode=="success") {
            QVERIFY(done.last()[0].toBool());QCOMPARE(loaded.size(),1);QVERIFY(ticks>0);
            QCOMPARE(model(window)->command(1000)["query_result"]["query_type"],nlohmann::json(0));
            window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
            window.findChild<QLineEdit *>("apiSearch")->setText("GetData");
            auto table=window.findChild<QTableView *>("apiLog");QCOMPARE(table->model()->rowCount(),20000);
            table->setCurrentIndex(table->model()->index(0,0));
            auto properties=window.findChild<QTreeWidget *>("properties");QVERIFY(properties);
            QVERIFY(!properties->findItems("query_result",Qt::MatchExactly).empty());
            qInfo()<<"Query preparation event-loop ticks:"<<ticks;
        } else {
            QVERIFY(!done.last()[0].toBool());QCOMPARE(loaded.size(),0);QCOMPARE(window.capturePath(),baseline);
            QCOMPARE(model(window)->command(1000),expected);
            QFile released(large);QVERIFY2(released.open(QIODevice::ReadWrite),"Cancelled candidate capture is still retained");
        }
        window.show();done.clear();window.openCapture(baseline);QTRY_VERIFY_WITH_TIMEOUT(!done.empty()&&!window.busy(),30000);
        QVERIFY(done.last()[0].toBool());QCOMPARE(model(window)->command(1000),expected);
        // Reusing the same IDs in another capture must replace the prepared rows.
        if(mode=="switch") {
            done.clear();window.openCapture(large);QTRY_VERIFY_WITH_TIMEOUT(!done.empty()&&!window.busy(),60000);
            QVERIFY(done.last()[0].toBool());QCOMPARE(model(window)->command(1000)["query_result"]["query_type"],nlohmann::json(0));
        }
    }
    void original_data() { QTest::addColumn<QString>("name");QTest::newRow("gf2")<<QString("GF2_Exilium_2026_03_03__00_19_35.gpa_frame");QTest::newRow("bf1")<<QString("bf1_2026_01_21__16_53_05.gpa_frame"); }
    void original() {
        QFETCH(QString,name);const auto root=qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");if(root.isEmpty())QSKIP("Set FLORA_TEST_CAPTURE_DIR for original Query navigation");
        const auto path=root+'/'+name;auto frame=std::make_shared<Frame>(path.toStdWString());auto expected=QueryInspection::prepare(frame);
        MainWindow window;window.show();QSignalSpy done(&window,&MainWindow::taskFinished);
        window.openCapture(path);QTRY_VERIFY_WITH_TIMEOUT(!done.empty()&&!window.busy(),30000);QVERIFY(done.last()[0].toBool());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);window.findChild<QLineEdit *>("apiSearch")->setText("GetData");
        auto table=window.findChild<QTableView *>("apiLog");QCOMPARE(size_t(table->model()->rowCount()),expected->size());QVERIFY(expected->size()>0);
        for(int row=0;row<table->model()->rowCount();++row) {
            const auto index=table->model()->index(row,0);const auto id=index.data(Qt::UserRole).toULongLong();
            table->setCurrentIndex(index);QCOMPARE(model(window)->command(id),*expected->find(id));
            auto properties=window.findChild<QTreeWidget *>("properties");QVERIFY(properties);
            QVERIFY(!properties->findItems("query_result",Qt::MatchExactly).empty());
        }
    }
    void predicate_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("occlusion-true")<<1;
        QTest::newRow("overflow-false")<<8;
    }
    void predicate() {
        QFETCH(int,mode);
        const auto root=qEnvironmentVariable("FLORA_PREDICATE_CREATION_CAPTURES");
        if(root.isEmpty())QSKIP("Set FLORA_PREDICATE_CREATION_CAPTURES for original Predicate navigation");
        MainWindow window;window.show();QSignalSpy done(&window,&MainWindow::taskFinished);
        window.openCapture(root+'/'+QString::number(mode)+"/capture.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty()&&!window.busy(),30000);QVERIFY(done.last()[0].toBool());
        window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(1);
        window.findChild<QLineEdit *>("apiSearch")->setText("GetData");
        auto table=window.findChild<QTableView *>("apiLog");
        auto properties=window.findChild<QTreeWidget *>("properties");QVERIFY(properties);
        size_t complete=0;
        for(int row=0;row<table->model()->rowCount();++row) {
            const auto index=table->model()->index(row,0);const auto id=index.data(Qt::UserRole).toULongLong();
            table->setCurrentIndex(index);const auto details=model(window)->command(id);
            if(!details.contains("query_result"))continue; // GetDataSize also matches the filter.
            const auto &query=details["query_result"];QVERIFY(query["issues"].empty());
            QCOMPARE(query["query_type"],nlohmann::json(mode==8?7:5));
            bool desc=false,size=false;
            for(const auto &metadata:query["metadata"]) {
                if(metadata["event"].is_null())continue;
                const auto getter=model(window)->command(metadata["event"].get<Id>());
                QVERIFY(metadata["event"].get<Id>()<id);
                if(metadata["source"]=="same_id_GetDesc") {QCOMPARE(getter["type"],nlohmann::json(0x316a));desc=true;}
                if(metadata["source"]=="same_id_GetDataSize") {QCOMPARE(getter["type"],nlohmann::json(0x3169));size=true;}
            }
            QVERIFY(desc&&size);
            // Check the visible properties tree, not just the prepared model.
            auto items=properties->findItems("source",Qt::MatchExactly|Qt::MatchRecursive);
            bool visibleDesc=false,visibleSize=false;
            for(auto item:items) {
                visibleDesc|=item->text(1)=="same_id_GetDesc";
                visibleSize|=item->text(1)=="same_id_GetDataSize";
            }
            QVERIFY(visibleDesc&&visibleSize);
            if(query["status"]=="complete") {++complete;QCOMPARE(query["fields"][0]["value"],nlohmann::json(mode==1));}
            else QCOMPARE(query["status"],nlohmann::json("not_ready"));
        }
        QVERIFY(complete>0);
    }
};
QTEST_MAIN(QueryInspectionUiTests)
#include "QueryInspectionUiTests.moc"
