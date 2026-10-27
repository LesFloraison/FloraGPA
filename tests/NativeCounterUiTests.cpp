#include "DepthStencilCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/NativeAnalysisController.h"
#include "app/NativeCountersView.h"
#include "application/NativeCounters.h"
#include <QAction>
#include <QFile>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
QAction *action(QWidget &view, const char *name) { return view.findChild<QAction *>(name); }
Json fixture() {
    return {
        {"ok", true},
        {"data",
         {{"schema", 1},
          {"kind", "event_counters"},
          {"source", "native_dx11_queries"},
          {"catalog",
           Json::array(
               {{{"id", "input.vertices"}, {"name", "Input vertices"}, {"type", "uint64"}, {"unit", "count"}},
                {{"id", "gpu.duration"},
                 {"name", "GPU duration"},
                 {"type", "float64"},
                 {"unit", "seconds"}}})},
          {"events", Json::array({{{"event", 100},
                                   {"enabled", true},
                                   {"timing_valid", false},
                                   {"values", {{"input.vertices", UINT64_MAX}, {"gpu.duration", nullptr}}}},
                                  {{"event", uint64_t(1) << 40},
                                   {"enabled", true},
                                   {"timing_valid", true},
                                   {"values", {{"input.vertices", 16777219}, {"gpu.duration", 0.25}}}}})}}}};
}
void noExternalModules(const Json &result) {
    for (const auto &module : result.at("execution").at("loaded_modules")) {
        const auto name = QString::fromStdString(module.get<std::string>()).toLower();
        QVERIFY(!name.contains("renderdoc"));
        QVERIFY(!name.contains("intelswtools\\gpa\\"));
    }
}
} // namespace
class NativeCounterUiTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-NativeCounterTests");
        QSettings().clear();
        QSettings().setValue("analysis/renderdoc", "C:/missing/renderdoc.dll");
        applyAppearance(*qApp);
    }
    void presentation() {
        NativeCountersView view;
        view.setContext("frame", 100);
        const auto serial = view.requestId();
        QVERIFY(view.finish(serial, fixture()));
        auto values = view.findChild<QTreeWidget *>("nativeCounterValues");
        QCOMPARE(values->topLevelItemCount(), 2);
        QCOMPARE(values->topLevelItem(0)->text(1), QString("18446744073709551615"));
        QCOMPARE(values->topLevelItem(1)->text(1), QString("—"));
        action(view, "nativeCountersDetailsToggle")->trigger();
        QCOMPARE(Json::parse(
                     view.findChild<QPlainTextEdit *>("nativeCounterDetails")->toPlainText().toStdString()),
                 fixture());
        view.setContext("frame", uint64_t(1) << 40);
        QCOMPARE(view.requestId(), serial);
        QCOMPARE(values->topLevelItem(0)->text(1), QString("16777219"));
        QCOMPARE(values->topLevelItem(1)->text(1), QString("0.25"));
        QCOMPARE(values->topLevelItem(1)->text(2), QString("s"));
        values->setCurrentItem(values->topLevelItem(0));
        QSignalSpy locate(&view, &NativeCountersView::eventRequested);
        action(view, "locateNativeCounter")->trigger();
        QCOMPARE(locate.size(), 1);
        QCOMPARE(locate[0][0].toULongLong(), qulonglong(1) << 40);
        view.findChild<QComboBox *>("nativeCountersScope")->setCurrentIndex(1);
        QCOMPARE(values->topLevelItemCount(), 2);
        auto filter = view.findChild<QLineEdit *>("nativeCountersFilter");
        filter->setText("duration");
        QCOMPARE(values->topLevelItem(0)->childCount(), 1);
        auto catalog = view.findChild<QTreeWidget *>("nativeCounterCatalog");
        QCOMPARE(catalog->topLevelItemCount(), 1);
        catalog->setCurrentItem(catalog->topLevelItem(0));
        QCOMPARE(Json::parse(
                     view.findChild<QPlainTextEdit *>("nativeCounterDetails")->toPlainText().toStdString()),
                 fixture()["data"]["catalog"][1]);
        view.setWorkerBusy(true);
        QVERIFY(!action(view, "readNativeCounters")->isEnabled());
        QVERIFY(!action(view, "exportNativeCounters")->isEnabled());
        view.setWorkerBusy(false);
        auto invalid = fixture();
        invalid["data"]["events"][0]["values"]["input.vertices"] = -1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(serial, invalid));
        QCOMPARE(view.result(), fixture());
        invalid = fixture();
        invalid["data"]["events"][0]["timing_valid"] = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(serial, invalid));
        invalid = fixture();
        invalid["data"]["catalog"].push_back(invalid["data"]["catalog"][0]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(serial, invalid));
        QTemporaryDir directory;
        view.exportResult(directory.filePath("counters.json"));
        QFile exported(directory.filePath("counters.json"));
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(Json::parse(exported.readAll().toStdString()), fixture());
        view.setContext("changed experiment", 100);
        QVERIFY(view.result().is_null());
        QVERIFY(!view.finish(serial, fixture()));
        QCOMPARE(values->topLevelItemCount(), 0);
    }
    void controllerRecovery() {
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::depthStencilCapture().save(capture);
        const auto worker = QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe";
        NativeAnalysisController controller;
        NativeAnalysisOutcome outcome;
        int finishes = 0;
        connect(&controller, &NativeAnalysisController::finished, this, [&](const auto &value) {
            outcome = value;
            ++finishes;
        });
        controller.start(capture, worker, "counters", {{"warp", true}}, nullptr);
        QVERIFY(controller.active());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 controller.start(capture, worker, "inventory", {}, nullptr));
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 1, 30000);
        QVERIFY(!controller.active());
        QVERIFY(outcome.cancelled && outcome.result.is_null());
        controller.start(capture, directory.filePath("missing.exe"), "counters", {{"warp", true}}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 2, 30000);
        QVERIFY(!outcome.error.isEmpty() && outcome.result.is_null());
        controller.start(capture, worker, "counters", {{"warp", true}}, nullptr, 0.001);
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 3, 30000);
        QVERIFY(outcome.error.contains("timed out"));
        controller.start(capture, worker, "counters", {{"warp", true}}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 4, 30000);
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(nativeCounterRows(outcome.result.at("data")).size(), size_t(52));
        QVERIFY(outcome.artifacts.contains("counters.csv"));
        QVERIFY(!QFileInfo::exists(QString::fromStdString(outcome.job.at("out"))));
        noExternalModules(outcome.result);
        // Cancel after the Worker returns but before the GUI consumes its queued completion.
        controller.start(capture, worker, "inventory", Json::object(), nullptr);
        QTest::qSleep(1500);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 5, 30000);
        QVERIFY(outcome.cancelled && outcome.result.is_null());
        controller.start(capture, worker, "debug-vertex", {{"gpa_event", 1000}, {"warp", true}}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 6, 30000);
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QVERIFY(outcome.debugSession);
        QCOMPARE(outcome.result.at("context").at("action"), Json("debug-vertex"));
        noExternalModules(outcome.result);
        // VS execution is implemented; the reserved PS entry still exercises
        // propagation of the original Worker diagnostic without a fallback.
        controller.start(capture, worker, "debug-pixel", {{"gpa_event", 1000}, {"warp", true}}, nullptr);
        QTRY_COMPARE_WITH_TIMEOUT(finishes, 7, 30000);
        QVERIFY(outcome.error.contains("not implemented"));
        QVERIFY(outcome.workerLog.contains("not implemented"));
    }
    void mainWindow_data() {
        QTest::addColumn<unsigned>("vendor");
        QTest::newRow("warp") << 0u;
        QTest::newRow("nvidia") << 0x10deu;
        QTest::newRow("intel") << 0x8086u;
    }
    void mainWindow() {
        QFETCH(unsigned, vendor);
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::depthStencilCapture().save(capture);
        MainWindow window;
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        const auto index = vendor ? adapter->findData(vendor) : 1;
        if (index < 0)
            QSKIP("Requested hardware adapter is not installed; WARP is covered separately");
        adapter->setCurrentIndex(index);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        auto view = window.findChild<NativeCountersView *>();
        QVERIFY(view);
        window.findChild<QTabWidget *>("inspectorTabs")->setCurrentWidget(view);
        auto api = window.findChild<QTableView *>("apiLog");
        auto select = [&](Id event) {
            for (int i = 0; i < api->model()->rowCount(); ++i)
                if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == event) {
                    api->setCurrentIndex(api->model()->index(i, 0));
                    return;
                }
            QFAIL("Missing synthetic API event");
        };
        select(1000);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        const auto original = view->result();
        QCOMPARE(original.at("data").at("events").size(), size_t(4));
        QCOMPARE(original.at("data").at("events")[2].at("values").at("input.vertices"), Json(3));
        QCOMPARE(original.at("context").at("adapter").at("warp"), Json(!vendor));
        if (vendor)
            QCOMPARE(original.at("context").at("adapter").at("vendor"), Json(vendor));
        noExternalModules(original);
        const auto token = view->requestId();
        select(2000);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QCOMPARE(view->requestId(), token);
        QCOMPARE(view->result(), original);
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        action(*view, "cancelNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        QVERIFY(view->result().is_null());
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(view->result().at("context_id"), original.at("context_id"));
        // Changing selection while hashing/measurement is active cancels the job.
        QSignalSpy inspections(view, &NativeCountersView::inspectionFinished);
        action(*view, "readNativeCounters")->trigger();
        select(1000);
        QTRY_VERIFY_WITH_TIMEOUT(!inspections.empty(), 30000);
        QVERIFY(!inspections.takeLast()[0].toBool());
        QVERIFY(view->result().is_null());
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        select(2000);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QAction *disable = nullptr, *undo = nullptr;
        for (auto item : window.findChildren<QAction *>()) {
            if (item->text() == "Disable Event")
                disable = item;
            if (item->text() == "Undo")
                undo = item;
        }
        QVERIFY(disable && undo);
        tasks.clear();
        disable->trigger();
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(view->result().is_null());
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QVERIFY(view->result().at("context_id") != original.at("context_id"));
        QCOMPARE(view->result().at("data").at("events")[3].at("values").at("input.vertices"), Json(0));
        undo->trigger();
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        QCOMPARE(view->result().at("data").at("events")[3].at("values").at("input.vertices"), Json(3));
        if (const auto root = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR"); !root.isEmpty()) {
            QVERIFY(QDir().mkpath(root));
            view->exportResult(root + QString("/native-counters-%1.json").arg(vendor));
            QVERIFY(window.grab().save(root + QString("/native-counters-%1.png").arg(vendor)));
        }
    }
};
QTEST_MAIN(NativeCounterUiTests)
#include "NativeCounterUiTests.moc"
