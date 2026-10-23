#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/RdcCountersView.h"
#include "app/NativeCountersView.h"
#include "app/ReplayDebugView.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing counter fixture");
    const auto bytes = file.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
QAction *action(QWidget &w, const char *name) { return w.findChild<QAction *>(name); }
QString text(const Json &j) {
    return QString::fromStdString(j.is_string() ? j.get<std::string>() : j.dump());
}
Json fixture() {
    return {{"ok", true},
            {"action", "counters"},
            {"result_count", 2},
            {"available", Json::array({{{"counter", 2},
                                        {"name", "Wide count"},
                                        {"category", "Test"},
                                        {"description", "Exact integer"},
                                        {"resultType", 4},
                                        {"resultByteWidth", 8}}})},
            {"values", Json::array({{{"eventId", 10},
                                     {"gpa_event", 100},
                                     {"counter", 2},
                                     {"name", "Wide count"},
                                     {"unit", "CounterUnit.Absolute"},
                                     {"value", UINT64_MAX}},
                                    {{"eventId", 20},
                                     {"gpa_event", nullptr},
                                     {"counter", 2},
                                     {"name", "Wide count"},
                                     {"unit", "CounterUnit.Absolute"},
                                     {"value", uint64_t(16777219)}}})}};
}
} // namespace
class RdcCounterUiTests : public QObject {
    Q_OBJECT
    QString output_;
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-CounterTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty()) {
            output_ += "/rdc-counters";
            QVERIFY(QDir().mkpath(output_));
        }
    }
    void exactValuesAndInvalidation() {
        RdcCountersView view;
        view.resize(480, 650);
        view.show();
        QVERIFY(!action(view, "readRdcCounters")->isEnabled());
        view.setContext("frame", 100);
        QCOMPARE(view.request(), Json({{"action", "counters"}}));
        const auto token = view.requestId();
        QVERIFY(view.finish(token, fixture()));
        auto rows = view.findChild<QTreeWidget *>("rdcCounterValues");
        QCOMPARE(rows->topLevelItemCount(), 1);
        QCOMPARE(rows->topLevelItem(0)->text(1), QString("18446744073709551615"));
        QVERIFY(view.findChild<QPlainTextEdit *>("rdcCounterDetails")->isHidden());
        action(view, "rdcCountersDetailsToggle")->trigger();
        QCOMPARE(
            Json::parse(view.findChild<QPlainTextEdit *>("rdcCounterDetails")->toPlainText().toStdString()),
            fixture());
        rows->setCurrentItem(rows->topLevelItem(0));
        QSignalSpy jump(&view, &RdcCountersView::eventRequested);
        action(view, "locateRdcCounter")->trigger();
        QCOMPARE(jump.size(), 1);
        QCOMPARE(jump[0][0].toULongLong(), qulonglong(100));
        view.setContext("frame", 101);
        QCOMPARE(view.requestId(), token);
        QCOMPARE(rows->topLevelItemCount(), 0);
        QCOMPARE(view.result(), fixture());
        view.findChild<QComboBox *>("rdcCountersScope")->setCurrentIndex(1);
        QCOMPARE(rows->topLevelItemCount(), 2);
        rows->setCurrentItem(rows->topLevelItem(1)->child(0));
        QVERIFY(!action(view, "locateRdcCounter")->isEnabled());
        view.setWorkerBusy(true);
        QVERIFY(!action(view, "readRdcCounters")->isEnabled());
        QVERIFY(!action(view, "exportRdcCounters")->isEnabled());
        view.setWorkerBusy(false);
        auto invalid = fixture();
        invalid["result_count"] = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(token, invalid));
        QCOMPARE(view.result(), fixture());
        QTemporaryDir temp;
        view.exportResult(temp.path() + "/counter.json");
        QCOMPARE(read(temp.path() + "/counter.json"), fixture());
        view.setContext("different experiment", 100);
        QVERIFY(view.result().is_null());
        QVERIFY(!view.finish(token, fixture()));
        QCOMPARE(rows->topLevelItemCount(), 0);
    }
    void recordedReports() {
        const auto root = qEnvironmentVariable("FLORA_COUNTER_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_COUNTER_REPORTS to the native/Python counter corpus");
        int verified = 0;
        size_t total = 0;
        for (const auto &dir : QDir(root).entryList({"*-native"}, QDir::Dirs)) {
            const auto report = read(root + '/' + dir + "/result.json");
            if (!report.value("ok", false))
                continue;
            RdcCountersView view;
            view.setContext("frame", 0);
            view.findChild<QComboBox *>("rdcCountersScope")->setCurrentIndex(1);
            QVERIFY(view.finish(view.requestId(), report));
            auto rows = view.findChild<QTreeWidget *>("rdcCounterValues");
            size_t count = 0;
            for (int i = 0; i < rows->topLevelItemCount(); ++i)
                for (int j = 0; j < rows->topLevelItem(i)->childCount(); ++j) {
                    auto item = rows->topLevelItem(i)->child(j);
                    const auto &record =
                        report.at("values").at(item->data(0, Qt::UserRole + 1).toULongLong());
                    QCOMPARE(item->text(1), text(record.at("value")));
                    QCOMPARE(item->text(0), text(record.at("name")));
                    ++count;
                }
            QCOMPARE(count, report.at("values").size());
            total += count;
            auto catalog = view.findChild<QTreeWidget *>("rdcCounterCatalog");
            QCOMPARE(catalog->topLevelItemCount(), int(report.at("available").size()));
            for (int i = 0; i < catalog->topLevelItemCount(); ++i) {
                catalog->setCurrentItem(catalog->topLevelItem(i));
                QCOMPARE(
                    Json::parse(
                        view.findChild<QPlainTextEdit *>("rdcCounterDetails")->toPlainText().toStdString()),
                    report.at("available").at(i));
            }
            view.findChild<QLineEdit *>("rdcCountersFilter")->setText("VS Invocations");
            for (int i = 0; i < rows->topLevelItemCount(); ++i)
                for (int j = 0; j < rows->topLevelItem(i)->childCount(); ++j)
                    QVERIFY(rows->topLevelItem(i)->child(j)->text(0).contains("VS Invocations",
                                                                              Qt::CaseInsensitive));
            QTemporaryDir temp;
            view.exportResult(temp.path() + "/counter.json");
            QCOMPARE(read(temp.path() + "/counter.json"), report);
            ++verified;
        }
        QVERIFY(verified >= 9);
        QVERIFY(total > 1000);
    }
    void mainWindow_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void mainWindow() {
        QFETCH(bool, warp);
        const auto capture = qEnvironmentVariable("FLORA_DEBUG_SOURCE_CAPTURE");
        if (capture.isEmpty())
            QSKIP("Set FLORA_DEBUG_SOURCE_CAPTURE to the original shader_sources capture");
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(capture);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        auto adapter = window.findChild<QComboBox *>("replayAdapter");
        if (adapter->currentIndex() != int(warp)) {
            adapter->setCurrentIndex(int(warp));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
        }
        auto api = window.findChild<QTableView *>("apiLog");
        auto select = [&](qulonglong event) {
            for (int i = 0; i < api->model()->rowCount(); ++i)
                if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == event) {
                    api->setCurrentIndex(api->model()->index(i, 0));
                    return;
                }
            QFAIL("Missing API event");
        };
        select(105);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto view = window.findChild<NativeCountersView *>();
        QVERIFY(view);
        window.findChild<QTabWidget *>("inspectorTabs")->setCurrentWidget(view);
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        const auto original = view->result();
        QVERIFY(original.at("data").at("events").size() > 0);
        bool known = false;
        for (const auto &r : original.at("data").at("events"))
            if (r["event"] == 105) {
                QCOMPARE(r["values"]["input.vertices"], Json(3));
                known = true;
            }
        QVERIFY(known);
        auto rows = view->findChild<QTreeWidget *>("nativeCounterValues");
        QVERIFY(rows->topLevelItemCount() > 0);
        if (!output_.isEmpty()) {
            const auto suffix = warp ? QString("warp") : QString("hardware");
            view->exportResult(output_ + "/counters-" + suffix + ".json");
            QVERIFY(window.grab().save(output_ + "/metrics-" + suffix + ".png"));
        }
        const auto token = view->requestId();
        select(51);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        QCOMPARE(view->requestId(), token);
        QCOMPARE(view->result(), original);
        QVERIFY(rows->topLevelItemCount() > 0);
        view->findChild<QComboBox *>("nativeCountersScope")->setCurrentIndex(1);
        QTreeWidgetItem *draw = nullptr;
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() == 105) {
                draw = rows->topLevelItem(i);
                break;
            }
        QVERIFY(draw);
        rows->setCurrentItem(draw);
        window.findChild<QLineEdit *>("apiSearch")->setText("no match");
        action(*view, "locateNativeCounter")->trigger();
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(105));
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        QCOMPARE(view->result(), original);
        // Legacy debugging uses its own capture; native full-frame metrics survive that job.
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-ps");
        debug->selectPixel(48, 32, 0);
        debug->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*debug, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(debug->exportReport().at("frame_sha256"), original.at("context").at("frame_sha256"));
        QCOMPARE(view->result(), original);
        if (!output_.isEmpty())
            debug->exportResult(output_ + (warp ? "/debug-warp.json" : "/debug-hardware.json"));
        action(*view, "readNativeCounters")->trigger();
        action(*view, "cancelNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().is_null());
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->result().at("context_id"), original.at("context_id"));
        QAction *disable{}, *undo{};
        for (auto a : window.findChildren<QAction *>()) {
            if (a->text() == "Disable Event")
                disable = a;
            if (a->text() == "Undo")
                undo = a;
        }
        QVERIFY(disable && undo);
        disable->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().is_null());
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().at("context_id") != original.at("context_id"));
        for (const auto &r : view->result().at("data").at("events"))
            if (r.at("event") == 105) {
                QVERIFY(!r.at("enabled").get<bool>());
                QCOMPARE(r.at("values").at("input.vertices"), Json(0));
            }
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->result().at("context").at("effective_experiment").at("cursor"), Json(0));
        QSettings().setValue("analysis/renderdoc", "C:/missing/renderdoc.dll");
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "readNativeCounters")->trigger();
        select(51);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(view->result().is_null());
    }
};
QTEST_MAIN(RdcCounterUiTests)
#include "RdcCounterUiTests.moc"
