#include "BlendCapture.h"
#include "DepthStencilCapture.h"
#include "NativeDebugFixture.h"
#include "SyntheticCapture.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/NativeAnalysisController.h"
#include "app/NativeDebugControls.h"
#include "app/NativeExecutionDebugView.h"
#include <QAction>
#include <QComboBox>
#include <QLineEdit>
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
    void pixelSelectionAndWorker() {
        QTemporaryDir dir;
        const auto capture = dir.filePath("pixel.gpa_frame");
        testing::blendCapture().save(capture);
        NativeExecutionDebugView view;
        view.setContext("pixel", 100);
        auto stage = view.findChild<QComboBox *>("nativeDebugStage");
        auto resource = view.findChild<QLineEdit *>("nativeDebugIndex");
        auto primitive = view.findChild<QLineEdit *>("nativeDebugThread");
        auto pixel = view.findChild<QLineEdit *>("nativeDebugGroup");
        auto subresource = view.findChild<QLineEdit *>("nativeDebugPixelSubresource");
        QVERIFY(stage && resource && primitive && pixel && subresource);
        stage->setCurrentIndex(2);
        QCOMPARE(view.action(), std::string("debug-pixel"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        resource->setText("1000");
        QCOMPARE(view.request().at("debug_scope"), Json("private-primitive"));
        auto request = view.request();
        request["warp"] = true;
        NativeAnalysisController controller;
        QSignalSpy completed(&controller, &NativeAnalysisController::finished);
        controller.start(capture, QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe",
                         "debug-pixel", request, nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(!completed.empty(), 30000);
        const auto outcome = completed.takeFirst()[0].value<NativeAnalysisOutcome>();
        QVERIFY2(outcome.debugSession != nullptr, qPrintable(outcome.error));
        QVERIFY(view.finish(view.requestId(), outcome.debugSession));
        QVERIFY(view.findChild<QLabel *>("nativeDebugSummary")->text().contains("eligibility unknown"));
        const auto initial = view.model()->registerValues();
        trigger(view, "nativeDebugNext");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        trigger(view, "nativeDebugPrevious");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->registerValues(), initial);
        const auto stale = view.requestId();
        subresource->setText("1,0,0");
        QVERIFY(view.requestId() != stale && !view.model());
        QVERIFY(!view.finish(stale, outcome.debugSession));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), outcome.debugSession));
        subresource->setText("0,0,0");
        for (const auto *invalid : {"-1", "+1", "4294967296", " 1"}) {
            primitive->setText(invalid);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        }
        primitive->setText("0");
        resource->setText("4294967297");
        QCOMPARE(view.request().at("resource"), Json(uint64_t(4294967297)));
        pixel->setText("1,2,3");
        QCOMPARE(view.request().at("sample"), Json(3));
        stage->setCurrentIndex(1);
        QCOMPARE(view.action(), std::string("debug-vertex"));
        QVERIFY(!view.request().contains("resource"));
    }
    void vertexSelectionAndNavigation() {
        NativeExecutionDebugView view;
        view.setContext("vertex", 19);
        auto stage = view.findChild<QComboBox *>("nativeDebugStage");
        QVERIFY(stage);
        stage->setCurrentIndex(1);
        QCOMPARE(view.action(), std::string("debug-vertex"));
        QCOMPARE(view.request(), Json({{"gpa_event", 19}, {"vertex", 0}, {"instance", 0}}));
        test::NativeDebugFixture f(true);
        auto selected = std::make_shared<NativeDebugSession>(f.job, f.result, f.artifacts);
        QVERIFY(view.finish(view.requestId(), selected));
        QCOMPARE(view.exportReport().at("invocation"), f.result.at("data").at("invocation"));
        const auto initial = view.model()->registerValues();
        auto interpretation = view.findChild<QComboBox *>("nativeDebugInterpretation");
        QVERIFY(interpretation && interpretation->findText("double") >= 0);
        interpretation->setCurrentText("double");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->registerValues(), initial);
        auto registers = view.findChild<QTreeWidget *>("nativeDebugRegisters");
        QVERIFY(registers);
        const auto doubleRows = view.model()->registerRows("double");
        QCOMPARE(size_t(registers->topLevelItemCount()), doubleRows.size());
        for (int i = 0; i < registers->topLevelItemCount(); ++i)
            QCOMPARE(registers->topLevelItem(i)->text(1), QString::fromStdString(doubleRows[i][1]));
        trigger(view, "nativeDebugNext");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->position(), size_t(1));
        trigger(view, "nativeDebugPrevious");
        QTRY_VERIFY_WITH_TIMEOUT(!view.operationActive(), 10000);
        QCOMPARE(view.model()->registerValues(), initial);
        const auto stale = view.requestId();
        stage->setCurrentIndex(0);
        QVERIFY(!view.model());
        QVERIFY(!view.finish(stale, selected));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), selected));
    }
    void vertexIndexSelection() {
        NativeExecutionDebugView view;
        view.setContext("vertex-index", 19);
        auto stage = view.findChild<QComboBox *>("nativeDebugStage");
        auto index = view.findChild<QLineEdit *>("nativeDebugIndex");
        QVERIFY(stage && index);
        stage->setCurrentIndex(1);
        const auto before = view.requestId();
        index->setText("0");
        QVERIFY(view.requestId() != before);
        QCOMPARE(view.request().at("index"), Json(0));
        test::NativeDebugFixture f(true);
        auto selected = std::make_shared<NativeDebugSession>(f.job, f.result, f.artifacts);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.finish(view.requestId(), selected));
        for (const auto *bad : {"-1", "+1", "4294967296", " 1", "1 ", "x"}) {
            index->setText(bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, view.request());
        }
        index->setText("4294967295");
        QCOMPARE(view.request().at("index"), Json(UINT32_MAX));
        view.setWorkerBusy(true);
        QVERIFY(!index->isEnabled());
        view.setWorkerBusy(false);
        index->clear();
        QVERIFY(!view.request().contains("index"));
        QVERIFY(view.finish(view.requestId(), selected));
        stage->setCurrentIndex(0);
        QVERIFY(!view.request().contains("index"));
    }
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
        QTest::addColumn<unsigned>("stage");
        QTest::newRow("warp") << 0u << 0u;
        QTest::newRow("nvidia") << 0x10deu << 0u;
        QTest::newRow("intel") << 0x8086u << 0u;
        QTest::newRow("vertex-warp") << 0u << 1u;
        QTest::newRow("vertex-nvidia") << 0x10deu << 1u;
        QTest::newRow("vertex-intel") << 0x8086u << 1u;
        QTest::newRow("pixel-warp") << 0u << 2u;
        QTest::newRow("pixel-nvidia") << 0x10deu << 2u;
        QTest::newRow("pixel-intel") << 0x8086u << 2u;
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
        QFETCH(unsigned, stage);
        QTemporaryDir directory;
        const auto capture = directory.filePath("frame.gpa_frame");
        (stage == 2   ? testing::blendCapture()
         : stage == 1 ? testing::depthStencilCapture()
                      : testing::computeCapture(true))
            .save(capture);
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
        view->findChild<QComboBox *>("nativeDebugStage")->setCurrentIndex(int(stage));
        if (stage == 2)
            view->findChild<QLineEdit *>("nativeDebugIndex")->setText("1000");
        auto tabs = window.findChild<QTabWidget *>("checkpointStageTabs");
        QCOMPARE(tabs->tabText(tabs->indexOf(view)), QString("Native execution"));
        tabs->setCurrentWidget(view);
        auto api = window.findChild<QTableView *>("apiLog");
        bool selected = false;
        for (int i = 0; i < api->model()->rowCount(); ++i)
            if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == (stage == 1 ? 1000 : 100)) {
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
