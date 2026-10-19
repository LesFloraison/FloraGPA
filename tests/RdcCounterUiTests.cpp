#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/RdcCountersView.h"
#include "app/ReplayDebugView.h"
#include "application/ReplayRequest.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
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
    void counterCatalogSelection() {
        const auto path = qEnvironmentVariable("FLORA_COUNTER_CATALOG_REPORT");
        if (path.isEmpty()) QSKIP("Set FLORA_COUNTER_CATALOG_REPORT to a saved DX12 catalog");
        const auto report = read(path);
        RdcCountersView view;
        view.resize(1100, 700); view.show();
        view.setContext("dx12:catalog", 21895, true);
        QVERIFY(view.nativeMode());
        auto source = view.findChild<QComboBox *>("rdcCountersSource");
        source->setCurrentIndex(1);
        QCoreApplication::processEvents();
        QVERIFY(source->isVisible());
        QVERIFY(!view.nativeMode());
        QVERIFY(action(view, "rdcCountersBackend")->isEnabled());
        QSignalSpy requested(&view, &RdcCountersView::readRequested);
        action(view, "listRdcCounters")->trigger();
        QCOMPARE(requested.size(), 1);
        QCOMPARE(view.request(), Json({{"action", "counter-catalog"}}));
        const auto catalogToken = view.requestId();
        QVERIFY(view.finish(catalogToken, report));
        auto catalog = view.findChild<QTreeWidget *>("rdcCounterCatalog");
        QCOMPARE(catalog->topLevelItemCount(), int(report.at("available").size()));
        auto ids = view.findChild<QLineEdit *>("rdcCounterIds");
        QTreeWidgetItem *valid{}, *diagnostic{};
        for (int i = 0; i < catalog->topLevelItemCount(); ++i) {
            const auto item = catalog->topLevelItem(i);
            if (item->text(3) == "1") valid = item;
            if (item->text(3) == "3000000") diagnostic = item;
        }
        QVERIFY(valid && diagnostic);
        QCOMPARE(diagnostic->text(6), QString("Diagnostic / unsupported"));
        catalog->itemDoubleClicked(diagnostic, 0);
        QVERIFY(ids->text().isEmpty());
        catalog->itemDoubleClicked(valid, 0);
        QCOMPARE(ids->text(), QString("1"));
        QVERIFY(!view.finish(catalogToken, report));
        QVERIFY(view.findChild<QLabel *>("rdcCountersSummary")->text().contains("not sampled"));
        if (!output_.isEmpty()) QVERIFY(view.grab().save(output_ + "/dx12-counter-catalog.png"));
        action(view, "readRdcCounters")->trigger();
        QCOMPARE(view.request(), Json({{"action", "counters"}, {"execution", "21895"}, {"counter_ids", Json::array({1})}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), report));
        auto measured = fixture();
        measured["backend"] = "dx12"; measured["execution"] = "21895";
        measured["counter_ids"] = Json::array({2});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), measured));
        measured["counter_ids"] = Json::array({1});
        measured["sampling_scope"] = "capture_actions";
        measured["values"] = Json::array({{{"eventId", 10}, {"execution", "21895"}, {"selectable", true},
            {"counter", 1}, {"name", "GPU Duration"}, {"source", "renderdoc_builtin"},
            {"unit", "CounterUnit.Seconds"}, {"value", 0.000001}}});
        measured["result_count"] = 1;
        QVERIFY(view.finish(view.requestId(), measured));
        QVERIFY(view.findChild<QLabel *>("rdcCountersSummary")->text().contains("capture actions"));
        source->setCurrentIndex(0);
        QVERIFY(view.result().is_null());
        QVERIFY(view.nativeMode());
        view.setContext("dx11", 100, false);
        QCOMPARE(view.request(), Json({{"action", "counters"}}));
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
    void nativeDx12Report() {
        const auto path = qEnvironmentVariable("FLORA_DX12_NATIVE_METRICS");
        if (path.isEmpty())
            QSKIP("Set the recorded native DX12 metrics report");
        auto report = read(path);
        RdcCountersView view;
        view.setContext("bf1-native", 21895, true);
        const auto identity = replayIdentityJson({"native-metrics", ReplayBackend::Dx12, 9007199254740993ULL, 7});
        const Json raw{{"completed", true}, {"backend", "dx12"}, {"execution", "21895"},
                       {"performance", report.at("native_measurement")}, {"device", report.at("device")},
                       {"request_identity", identity}, {"capture_identity", {{"kind", "runtime_session"}, {"value", "native-metrics"}}}};
        QVERIFY(view.finishNative(view.requestId(), raw));
        QCOMPARE(view.result().at("request_identity"), identity);
        if (!output_.isEmpty() && report.at("native_measurement").contains("timeline")) {
            view.resize(1100, 650); view.findChild<QTabWidget *>("rdcCounterTabs")->setCurrentIndex(2); view.show();
            QTest::qWait(30); QVERIFY(view.grab().save(output_ + "/dx12-timeline.png"));
        }
        QCOMPARE(view.result().at("capture_identity"), raw.at("capture_identity"));
        auto rows = view.findChild<QTreeWidget *>("rdcCounterValues");
        QCOMPARE(rows->topLevelItemCount(), 12);
        QCOMPARE(view.findChild<QComboBox *>("rdcCountersScope")->itemText(1), QString("Measured events"));
        QVERIFY(!action(view, "rdcCountersBackend")->isEnabled());
        const auto serial = view.requestId();
        view.setContext("bf1-native", 21622, true);
        QVERIFY(!view.finish(serial, report));
        QVERIFY(view.result().is_null());
        view.setContext("wide-native", 9007199254740993ULL, true);
        report["execution"] = "9007199254740993";
        for (auto &row : report["values"])
            row["execution"] = report["execution"];
        report["values"][1]["value"] = "18446744073709551615";
        QVERIFY(view.finish(view.requestId(), report));
        QCOMPARE(rows->topLevelItem(1)->text(1), QString("18446744073709551615"));
        rows->setCurrentItem(rows->topLevelItem(1));
        QSignalSpy jump(&view, &RdcCountersView::eventRequested);
        action(view, "locateRdcCounter")->trigger();
        QCOMPARE(jump.count(), 1);
        QCOMPARE(jump[0][0].toULongLong(), 9007199254740993ULL);
        report["values"][1]["execution"] = 9007199254740993ULL;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), report));
    }
    void nativeTimelineFallback() {
        const auto path = qEnvironmentVariable("FLORA_DX12_NATIVE_METRICS");
        if (path.isEmpty()) QSKIP("Set a recorded native timeline report");
        auto report = read(path);
        auto measurement = report.at("native_measurement");
        auto &timeline = measurement.at("timeline");
        auto row = timeline.at("entries").at(0);
        // Force unavailable calibration and adjacent ticks above 2^53. This is
        // a CPU display/identity boundary check, not another GPU measurement.
        row["queue"] = "9007199254740993"; row["first_execution"] = "9007199254740993";
        row["execution_count"] = "1";
        row["timestamp_begin"] = "9007199254740993"; row["timestamp_end"] = "9007199254740994";
        row["timestamp_frequency_hz"] = "1000000"; row["status"] = "measured";
        row["clock_scope"] = "queue_only"; row["begin_seconds"] = nullptr; row["end_seconds"] = nullptr;
        row["duration_seconds"] = 0.000001; row["reason"] = "Calibration unavailable";
        timeline["entries"] = Json::array({row});
        row["timestamp_begin"] = "9007199254740994"; row["timestamp_end"] = "9007199254740995";
        timeline["entries"].push_back(row); timeline["synchronization"] = Json::array();
        measurement["execution"] = "9007199254740993";
        Json raw{{"completed", true}, {"backend", "dx12"}, {"execution", "9007199254740993"},
                 {"performance", measurement}, {"device", report.at("device")}};
        RdcCountersView view; view.setContext("timeline", 9007199254740993ULL, true);
        QVERIFY(view.finishNative(view.requestId(), raw));
        auto tree = view.findChild<QTreeWidget *>("rdcReplayTimeline");
        QCOMPARE(tree->topLevelItemCount(), 1); const auto queue = tree->topLevelItem(0);
        QCOMPARE(queue->childCount(), 2);
        QCOMPARE(queue->child(0)->text(3), QString("0.000000"));
        QCOMPARE(queue->child(1)->text(3), QString("0.001000"));
        QCOMPARE(queue->child(1)->text(4), QString("1.000"));
        QCOMPARE(queue->child(1)->text(2), QString("Queue 9007199254740993"));
        QSignalSpy jump(&view, &RdcCountersView::eventRequested);
        emit tree->itemDoubleClicked(queue->child(0), 0);
        QCOMPARE(jump.count(), 1); QCOMPARE(jump[0][0].toULongLong(), 9007199254740993ULL);
        QTemporaryDir temp; view.exportResult(temp.filePath("timeline.json"));
        QCOMPARE(read(temp.filePath("timeline.json")).at("native_measurement"), measurement);
        auto invalid = raw; invalid["performance"]["timeline"]["entries"][0]["timestamp_begin"] = 9007199254740993ULL;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finishNative(view.requestId(), invalid));
        const auto token = view.requestId(); view.setContext("other experiment", 9007199254740993ULL, true);
        QVERIFY(!view.finishNative(token, raw)); QCOMPARE(tree->topLevelItemCount(), 0);
        view.setContext("dx11", 100, false);
        QVERIFY(!view.findChild<QTabWidget *>("rdcCounterTabs")->isTabVisible(2));
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
        auto view = window.findChild<RdcCountersView *>();
        QVERIFY(view);
        window.findChild<QTabWidget *>("inspectorTabs")->setCurrentWidget(view);
        view->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*view, "readRdcCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        const auto original = view->result();
        QVERIFY(original.at("values").size() > 0);
        bool known = false;
        for (const auto &r : original["values"])
            if (r["gpa_event"] == 105 && r["counter"] == 2) {
                QCOMPARE(r["value"], Json(3));
                known = true;
            }
        QVERIFY(known);
        auto rows = view->findChild<QTreeWidget *>("rdcCounterValues");
        QVERIFY(rows->topLevelItemCount() > 0);
        if (!output_.isEmpty()) {
            const auto suffix = warp ? QString("warp") : QString("hardware");
            view->exportResult(output_ + "/counters-" + suffix + ".json");
            const auto path = QString::fromStdString(original.at("capture").get<std::string>());
            const auto bytes = read(QFileInfo(path).dir().filePath("report.json")).dump(2);
            QSaveFile f(output_ + "/capture-" + suffix + ".json");
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
            QVERIFY(f.commit());
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
        view->findChild<QComboBox *>("rdcCountersScope")->setCurrentIndex(1);
        QTreeWidgetItem *draw = nullptr;
        for (int i = 0; i < rows->topLevelItemCount(); ++i)
            if (rows->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() == 105) {
                draw = rows->topLevelItem(i);
                break;
            }
        QVERIFY(draw);
        rows->setCurrentItem(draw);
        window.findChild<QLineEdit *>("apiSearch")->setText("no match");
        action(*view, "locateRdcCounter")->trigger();
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), qulonglong(105));
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        QCOMPARE(view->result(), original);
        // Debugging shares the capture, while the full-frame metrics survive the job.
        auto debug = window.findChild<ReplayDebugView *>("replayDebug-ps");
        debug->selectPixel(48, 32, 0);
        debug->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*debug, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(debug->exportReport().at("capture"), original.at("capture"));
        QCOMPARE(view->result(), original);
        if (!output_.isEmpty())
            debug->exportResult(output_ + (warp ? "/debug-warp.json" : "/debug-hardware.json"));
        action(*view, "readRdcCounters")->trigger();
        action(*view, "cancelRdcCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().is_null());
        action(*view, "readRdcCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->result().at("capture"), original.at("capture"));
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
        action(*view, "readRdcCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(view->result().at("capture") != original.at("capture"));
        for (const auto &r : view->result().at("values"))
            QVERIFY(r.at("gpa_event") != 105);
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "readRdcCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->result().at("capture"), original.at("capture"));
        view->setBackendPath("C:/missing/renderdoc.dll");
        action(*view, "readRdcCounters")->trigger();
        QCOMPARE(tasks.size(), 1);
        QVERIFY(!tasks.takeLast()[0].toBool());
        view->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*view, "readRdcCounters")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "readRdcCounters")->trigger();
        select(51);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(view->result().is_null());
    }
};
QTEST_MAIN(RdcCounterUiTests)
#include "RdcCounterUiTests.moc"
