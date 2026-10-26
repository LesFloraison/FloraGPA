#include "NativeDebugFixture.h"
#include "SyntheticCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/NativeDebugControls.h"
#include "app/NativeExecutionDebugView.h"
#include <QAction>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
void trigger(QWidget &view, const char *name) {
    auto action = view.findChild<QAction *>(name);
    if (!action || !action->isEnabled())
        throw std::runtime_error("Missing or disabled native debug action");
    action->trigger();
}
std::shared_ptr<NativeDebugSession> session() {
    test::NativeDebugFixture fixture;
    return std::make_shared<NativeDebugSession>(fixture.job, fixture.result, fixture.artifacts);
}
} // namespace
class NativeExecutionDebugUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-NativeExecutionDebugTests");
        QSettings().clear();
        applyAppearance(*qApp);
    }
    void presentationAndSource() {
        NativeExecutionDebugView view;
        QSignalSpy errors(&view, &NativeExecutionDebugView::error);
        view.setContext("frame", 19);
        QVERIFY(view.finish(view.requestId(), session()));
        QVERIFY(view.model());
        const auto initial = view.model()->registerValues();
        trigger(view, "nativeDebugNext");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->position(), size_t(1));
        trigger(view, "nativeDebugPrevious");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->position(), size_t(0));
        QCOMPARE(view.model()->registerValues(), initial);
        auto source = view.findChild<QPlainTextEdit *>("nativeDebugSource");
        QVERIFY(source->toPlainText().contains("uint total = index + 10"));
        size_t selected = 0;
        for (size_t i = 0; i < view.model()->size(); ++i) {
            view.move(i);
            QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
            bool found = false;
            for (const auto &value : view.model()->sourceValues())
                found |= value.at("name") == "total" && value.at("status") == "available";
            if (found) {
                selected = i;
                break;
            }
        }
        QVERIFY(selected > 0);
        QVERIFY(source->textCursor().hasSelection());
        auto controls = view.findChild<NativeDebugControls *>();
        auto watch = controls->findChild<QLineEdit *>("debugWatchExpression");
        watch->setText("total + 1");
        controls->findChild<QPushButton *>("debugAddWatch")->click();
        QCOMPARE(controls->watchResults().at(0).at("status"), Json("available"));
        const auto expected = view.model()->evaluate(selected, "total + 1").text();
        QCOMPARE(controls->watchResults().at(0).at("text"), Json(expected));
        const auto report = view.exportReport();
        QCOMPARE(report.at("watches").at(0).at("value"), Json(expected));
        const auto config = controls->configuration();
        auto invalid = config;
        invalid["format"] = "FloraGPA debug config 1";
        QVERIFY_THROWS_EXCEPTION(std::exception, controls->importConfiguration(invalid));
        QCOMPARE(controls->configuration(), config);
        const auto exported = qEnvironmentVariable("FLORA_NATIVE_DEBUG_EXPORT");
        if (!exported.isEmpty()) {
            view.exportArchive(exported);
            QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        }
        const auto screenshot = qEnvironmentVariable("FLORA_NATIVE_DEBUG_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            view.resize(1500, 850);
            QVERIFY(view.grab().save(screenshot));
        }
        auto locals = view.findChild<QTreeWidget *>("nativeDebugLocals");
        QVERIFY(locals->topLevelItemCount() > 0);
        view.move(view.model()->size());
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QVERIFY(controls->watchResults().at(0).at("status") == "unavailable");
        QVERIFY(view.findChild<QLabel *>("nativeDebugSummary")->text().contains("Terminal"));
        view.move(0);
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->registerValues(), initial);
        QTemporaryDir directory;
        view.exportResult(directory.filePath("state.json"));
        QFile file(directory.filePath("state.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(Json::parse(file.readAll().toStdString()).at("registers"), initial);
        QCOMPARE(errors.size(), 0);
    }
    void breakpointsAndContextChanges() {
        NativeExecutionDebugView view;
        view.setContext("original", 19);
        auto retained = session();
        const auto path = retained->tracePath();
        QVERIFY(view.finish(view.requestId(), retained));
        retained.reset();
        view.move(1);
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        trigger(view, "nativeDebugBreakpoint");
        view.move(0);
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        trigger(view, "nativeDebugContinue");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->position(), size_t(1));
        auto controls = view.findChild<NativeDebugControls *>();
        controls->settings().instructions.clear();
        controls->settings().source.setRule(0, 6, "total == 11", "always", 1);
        view.move(0);
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        trigger(view, "nativeDebugContinue");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->evaluate(view.model()->position(), "total").text(), std::string("11"));
        const auto token = view.requestId();
        view.setContext("different-experiment", 19);
        QVERIFY(!view.model());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!view.finish(token, session()));
        const auto nextToken = view.requestId();
        view.findChild<QLineEdit *>("nativeDebugThread")->setText("1,0,0");
        QVERIFY(view.requestId() != nextToken);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), session()));
        QVERIFY(!view.model());
        view.findChild<QLineEdit *>("nativeDebugThread")->setText("bad");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        view.setWorkerBusy(true);
        QVERIFY(!view.findChild<QAction *>("nativeDebugRead")->isEnabled());
        QVERIFY(view.findChild<QAction *>("nativeDebugCancel")->isEnabled());
    }
    void mainWindow_data() {
        QTest::addColumn<unsigned>("vendor");
        QTest::newRow("warp") << 0u;
        QTest::newRow("nvidia") << 0x10deu;
        QTest::newRow("intel") << 0x8086u;
    }
    void navigationInvalidation() {
        NativeExecutionDebugView view;
        view.setContext("original", 19);
        auto retained = session();
        const auto path = retained->tracePath();
        QVERIFY(view.finish(view.requestId(), retained));
        retained.reset();
        QSignalSpy completed(&view, &NativeExecutionDebugView::operationFinished);
        view.move(view.model()->size());
        QVERIFY(view.operationActive());
        QVERIFY(!view.model());
        QVERIFY(view.findChild<QAction *>("nativeDebugCancel")->isEnabled());
        view.setContext("replacement", 19);
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QVERIFY(!view.model());
        QCOMPARE(completed.size(), 1);
        QVERIFY(!completed[0][0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 10000);
        QVERIFY(view.finish(view.requestId(), session()));
        view.move(1);
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->position(), size_t(1));
    }
    void mainWindow() {
        QFETCH(unsigned, vendor);
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        testing::computeCapture(true).save(capture);
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
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
        auto view = window.findChild<NativeExecutionDebugView *>();
        QVERIFY(view);
        auto tabs = window.findChild<QTabWidget *>("checkpointStageTabs");
        tabs->setCurrentWidget(view);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == 100) {
                api->setCurrentIndex(api->model()->index(i, 0));
                selected = true;
                break;
            }
        QVERIFY(selected);
        QTest::qWait(300);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QSignalSpy inspected(view, &NativeExecutionDebugView::inspectionFinished);
        trigger(*view, "nativeDebugRead");
        QTRY_VERIFY_WITH_TIMEOUT(!inspected.empty(), 30000);
        QVERIFY2(inspected.takeLast()[0].toBool(),
                 qPrintable(view->findChild<QLabel *>("nativeDebugSummary")->text()));
        QVERIFY(view->model());
        const auto report = view->exportReport();
        QCOMPARE(report.at("analysis_context").at("adapter").at("warp"), Json(!vendor));
        if (vendor)
            QCOMPARE(report.at("analysis_context").at("adapter").at("vendor"), Json(vendor));
        trigger(*view, "nativeDebugRead");
        trigger(*view, "nativeDebugCancel");
        QTRY_VERIFY_WITH_TIMEOUT(!inspected.empty(), 30000);
        QVERIFY(!inspected.takeLast()[0].toBool());
        QVERIFY(!view->model());
        trigger(*view, "nativeDebugRead");
        QTRY_VERIFY_WITH_TIMEOUT(!inspected.empty(), 30000);
        QVERIFY(inspected.takeLast()[0].toBool());
        QCOMPARE(view->exportReport().at("analysis_context_id"), report.at("analysis_context_id"));
    }
};
QTEST_MAIN(NativeExecutionDebugUiTests)
#include "NativeExecutionDebugUiTests.moc"
