#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/PixelHistoryView.h"
#include "app/ReplayDebugView.h"
#include "application/NativeDebugConfig.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
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
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing shader debug test report");
    const auto bytes = f.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
QAction *action(QWidget &view, const char *name) { return view.findChild<QAction *>(name); }
void selectFile(const QString &path) {
    QTimer::singleShot(50, qApp, [path] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                dialog->setDirectory(QFileInfo(path).absolutePath());
                auto filename = dialog->findChild<QLineEdit *>("fileNameEdit");
                QVERIFY(filename);
                filename->setText(QFileInfo(path).fileName());
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
    });
}
} // namespace
class ReplayDebugUiTests : public QObject {
    Q_OBJECT
    QString output_;
  private slots:
    void initTestCase() {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-ReplayDebugTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty()) {
            output_ += "/replay-debug";
            QVERIFY(QDir().mkpath(output_));
        }
    }
    void selectionsAndInvalidation() {
        ReplayDebugView ps("ps"), vs("vs"), cs("cs");
        QVERIFY(!action(ps, "replayDebugRead")->isEnabled());
        ps.setContext("one", 105);
        vs.setContext("one", 105);
        cs.setContext("one", 51);
        ps.selectPixel(48, 32, 2);
        QCOMPARE(ps.request(),
                 (Json{{"action", "debug-pixel"}, {"gpa_event", 105}, {"x", 48}, {"y", 32}, {"sample", 2}}));
        vs.findChild<QLineEdit *>("debugVertex")->setText("2");
        QCOMPARE(vs.request().at("vertex"), Json(2));
        vs.findChild<QLineEdit *>("debugInstance")->setText("4294967295");
        QCOMPARE(vs.request().at("instance"), Json(UINT32_MAX));
        vs.findChild<QLineEdit *>("debugInstance")->setText("4294967296");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, vs.request());
        cs.findChild<QLineEdit *>("debugGroup")->setText("4294967295 0 7");
        QCOMPARE(cs.request().at("group"), Json({UINT32_MAX, 0, 7}));
        for (const auto &bad : {"-1 0 0", "+1 0 0", "4294967296 0 0", "1 2", "x 0 0"}) {
            cs.findChild<QLineEdit *>("debugGroup")->setText(bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, cs.request());
        }
        const auto token = ps.requestId();
        ps.setContext("two", 105);
        QSignalSpy completed(&ps, &ReplayDebugView::inspectionFinished);
        QVERIFY(!ps.finish(token, {{"ok", false}, {"error", "stale"}}));
        QCOMPARE(completed.size(), 0);
        ps.setWorkerBusy(true);
        QVERIFY(!action(ps, "replayDebugRead")->isEnabled());
        QVERIFY(action(ps, "replayDebugCancel")->isEnabled());
        ps.setWorkerBusy(false);
        QVERIFY(action(ps, "replayDebugRead")->isEnabled());
    }
    void backendReports() {
        const auto root = qEnvironmentVariable("FLORA_REPLAY_DEBUG_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_REPLAY_DEBUG_REPORTS to the native shader debug corpus");
        int verified = 0, positions = 0;
        for (const auto &dir : QDir(root).entryList({"*-native"}, QDir::Dirs)) {
            const auto result = read(root + '/' + dir + "/result.json");
            if (!result.value("ok", false) || !result.contains("steps"))
                continue;
            const auto kind = result.at("action").get<std::string>();
            ReplayDebugView view(kind == "debug-pixel" ? "ps" : kind == "debug-vertex" ? "vs" : "cs");
            view.resize(1250, 660);
            view.show();
            view.setContext("fixture", 105);
            QSignalSpy errors(&view, &ReplayDebugView::error);
            QVERIFY(view.finish(view.requestId(), result));
            QVERIFY(view.findChild<QPlainTextEdit *>("replayDebugDetails")->isHidden());
            action(view, "replayDebugDetailsToggle")->trigger();
            QCOMPARE(Json::parse(
                         view.findChild<QPlainTextEdit *>("replayDebugDetails")->toPlainText().toStdString()),
                     result);
            action(view, "replayDebugDetailsToggle")->trigger();
            QCOMPARE(view.findChild<QPlainTextEdit *>("debugAssembly")->toPlainText().toStdString(),
                     result.at("disassembly").get<std::string>());
            auto step = view.findChild<QSpinBox *>("debugStepIndex");
            auto registers = view.findChild<QTreeWidget *>("debugRegisters");
            auto variables = view.findChild<QTreeWidget *>("debugSourceVariables");
            auto stack = view.findChild<QTreeWidget *>("debugCallstack");
            ReplayDebugModel expected(result);
            auto verify = [&](size_t position) {
                expected.move(position);
                QCOMPARE(view.model()->position(), int64_t(position));
                QCOMPARE(variables->topLevelItemCount(), int(expected.sourceValues().size()));
                QCOMPARE(stack->topLevelItemCount(), int(expected.stack(position).size()));
                const auto rows = expected.registerRows();
                QCOMPARE(registers->topLevelItemCount(), int(rows.size()));
                for (size_t i = 0; i < rows.size(); ++i)
                    for (int col = 0; col < 3; ++col)
                        QCOMPARE(registers->topLevelItem(int(i))->text(col).toStdString(),
                                 rows[i][col].get<std::string>());
                QCOMPARE(view.exportReport(), expected.exportReport());
                ++positions;
            };
            verify(0);
            action(view, "debugPrevious")->trigger();
            QCOMPARE(view.model()->position(), int64_t(0));
            for (size_t i = 1; i < expected.size(); ++i) {
                action(view, "debugStep")->trigger();
                verify(i);
            }
            action(view, "debugStep")->trigger();
            QCOMPARE(view.model()->position(), int64_t(expected.size() - 1));
            for (size_t i = expected.size() - 1; i > 0; --i) {
                action(view, "debugPrevious")->trigger();
                verify(i - 1);
            }
            for (const auto &mode : {"int", "uint", "hex"}) {
                view.findChild<QComboBox *>("debugInterpretation")->setCurrentText(mode);
                const auto rows = expected.registerRows(mode);
                for (size_t i = 0; i < rows.size(); ++i)
                    QCOMPARE(registers->topLevelItem(int(i))->text(1).toStdString(),
                             rows[i][1].get<std::string>());
            }
            step->setValue(int(expected.size() - 1));
            const auto position = view.model()->position();
            for (int i = 0; i < stack->topLevelItemCount(); ++i)
                stack->setCurrentItem(stack->topLevelItem(i));
            for (int i = 0; i < view.findChild<QComboBox *>("debugSourceFile")->count(); ++i)
                view.findChild<QComboBox *>("debugSourceFile")->setCurrentIndex(i);
            QCOMPARE(view.model()->position(), position);
            auto watch = view.findChild<QLineEdit *>("debugWatchExpression");
            watch->setText("2u + 3u");
            action(view, "debugAddWatch")->trigger();
            auto watches = view.findChild<QTreeWidget *>("debugWatchValues");
            QCOMPARE(watches->topLevelItemCount(), 1);
            QCOMPARE(watches->topLevelItem(0)->text(2), QString("5"));
            QTemporaryDir temp;
            const auto exported = temp.path() + "/trace.json";
            view.exportResult(exported);
            QCOMPARE(read(exported), view.exportReport());
            view.setWorkerBusy(true);
            QVERIFY(!action(view, "debugToggleBreakpoint")->isEnabled());
            QVERIFY(!action(view, "debugStep")->isEnabled());
            view.setWorkerBusy(false);
            QVERIFY2(errors.empty(), qPrintable(errors.empty() ? QString{} : errors[0][0].toString()));
            view.setContext("changed", 105);
            QVERIFY(!view.model());
            QCOMPARE(registers->topLevelItemCount(), 0);
            QCOMPARE(variables->topLevelItemCount(), 0);
            QVERIFY(!action(view, "replayDebugExport")->isEnabled());
            ++verified;
        }
        QVERIFY(verified >= 21);
        QVERIFY(positions >= 500);
    }
    void sourceRulesAndConfig() {
        const auto root = qEnvironmentVariable("FLORA_REPLAY_DEBUG_REPORTS");
        if (root.isEmpty())
            QSKIP("Set FLORA_REPLAY_DEBUG_REPORTS to the native shader debug corpus");
        ReplayDebugView view("ps");
        view.resize(1250, 660);
        view.show();
        view.setContext("fixture", 105);
        QVERIFY(view.finish(view.requestId(), read(root + "/loop-ps-hardware-native/result.json")));
        auto model = view.model();
        auto step = view.findChild<QSpinBox *>("debugStepIndex");
        QSignalSpy navigationErrors(&view, &ReplayDebugView::error);
        view.findChild<QComboBox *>("debugNavigation")->setCurrentIndex(1);
        for (size_t i = 0; i < model->size(); ++i) {
            for (const auto name : {"debugPrevious", "debugStep", "debugOver", "debugOut"}) {
                step->setValue(int(i));
                navigationErrors.clear();
                std::optional<size_t> expected;
                try {
                    if (QString(name) == "debugPrevious")
                        expected = model->nextSource(i, -1);
                    else if (QString(name) == "debugStep")
                        expected = model->nextSource(i, 1);
                    else
                        expected = model->functionStep(i, QString(name) == "debugOut").first;
                } catch (const std::exception &) {
                }
                action(view, name)->trigger();
                QCOMPARE(model->position(), int64_t(expected.value_or(i)));
                QCOMPARE(navigationErrors.size(), expected ? 0 : 1);
            }
        }
        step->setValue(0);
        view.findChild<QLineEdit *>("debugInstruction")
            ->setText(QString::fromStdString(model->result().at("steps").at(1).at("nextInstruction").dump()));
        action(view, "debugRunTo")->trigger();
        QCOMPARE(model->position(), int64_t(1));
        for (size_t i = 0; i < model->size(); ++i) {
            step->setValue(int(i));
            if (!model->location(i).is_null())
                break;
        }
        const auto position = model->position();
        QVERIFY(!model->location(position).is_null());
        auto condition = view.findChild<QLineEdit *>("debugCondition");
        condition->setText("true");
        view.findChild<QComboBox *>("debugHitMode")->setCurrentIndex(3);
        view.findChild<QLineEdit *>("debugHitCount")->setText("2");
        action(view, "debugApplyRule")->trigger();
        QCOMPARE(model->breakpoints().size(), size_t(1));
        QCOMPARE(model->breakpoints()[0].at("condition"), Json("true"));
        view.findChild<QLineEdit *>("debugWatchExpression")->setText("1u");
        action(view, "debugAddWatch")->trigger();
        const auto config = model->configuration();
        QTemporaryDir temp;
        const auto path = temp.path() + "/调试配置.json";
        view.findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(3);
        QSignalSpy saveErrors(&view, &ReplayDebugView::error);
        QVERIFY(action(view, "debugSave")->isEnabled());
        selectFile(path);
        action(view, "debugSave")->trigger();
        QVERIFY2(saveErrors.empty(),
                 qPrintable(saveErrors.empty() ? QString{} : saveErrors[0][0].toString()));
        QVERIFY(QFile::exists(path));
        QCOMPARE(read(path), config);
        auto rules = view.findChild<QTreeWidget *>("debugRules");
        rules->setCurrentItem(rules->topLevelItem(0));
        action(view, "debugRemoveRule")->trigger();
        QVERIFY(model->breakpoints().empty());
        selectFile(path);
        action(view, "debugImport")->trigger();
        QCOMPARE(model->configuration(), config);
        QCOMPARE(model->position(), position);
        navigationErrors.clear();
        std::optional<size_t> next;
        try {
            next = model->seek(position);
        } catch (const std::exception &) {
        }
        action(view, "debugContinue")->trigger();
        QCOMPARE(model->position(), int64_t(next.value_or(position)));
        QCOMPARE(navigationErrors.size(), next ? 0 : 1);
        const auto selectedRule = model->breakpoints().at(0);
        QCOMPARE(rules->topLevelItem(0)->text(3).toULongLong(),
                 qulonglong(model->encounterCount(model->position(),
                                                  {selectedRule.at("file"), selectedRule.at("line")})));
        step->setValue(int(position));
        auto bad = config;
        bad["watches"] = {"1", "1"};
        const auto badPath = temp.path() + "/invalid.json";
        writeNativeDebugConfig(badPath.toStdWString(), bad);
        QSignalSpy errors(&view, &ReplayDebugView::error);
        selectFile(badPath);
        action(view, "debugImport")->trigger();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(model->configuration(), config);
        QCOMPARE(model->position(), position);
        if (!output_.isEmpty()) {
            step->setValue(int((position + 1) % int64_t(model->size())));
            step->setValue(int(position));
            view.findChild<QTabWidget *>("debugCodeTabs")->setCurrentIndex(1);
            view.findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(3);
            QVERIFY(view.grab().save(output_ + "/loop-rules.png"));
        }
    }
    void independentFrame_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void independentFrame() {
        QFETCH(bool, warp);
        const auto source = qEnvironmentVariable("FLORA_DEBUG_SOURCE_CAPTURE");
        if (source.isEmpty())
            QSKIP("Set FLORA_DEBUG_SOURCE_CAPTURE to the original shader_sources capture");
        QStringList dialogs;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, this, [&] {
            for (auto w : QApplication::topLevelWidgets())
                if (auto box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
                    dialogs << box->text();
                    box->accept();
                }
        });
        dismiss.start(20);
        MainWindow window;
        window.resize(1600, 950);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(source);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
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
        auto stages = window.findChild<QTabWidget *>("checkpointStageTabs");
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(stages);
        QCOMPARE(stages->count(), 6);
        Json capture;
        for (const auto &stage : {"vs", "ps", "cs"}) {
            select(QString(stage) == "cs" ? 51 : 105);
            QTest::qWait(800);
            QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
            tasks.clear();
            auto view = window.findChild<ReplayDebugView *>(QString("replayDebug-") + stage);
            QVERIFY(view);
            stages->setCurrentWidget(view);
            if (QString(stage) == "vs")
                view->findChild<QLineEdit *>("debugVertex")->setText("2");
            if (QString(stage) == "ps")
                view->selectPixel(48, 32, 0);
            view->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
            action(*view, "replayDebugRead")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
            tasks.clear();
            QVERIFY(view->model());
            const auto report = view->exportReport();
            if (capture.is_null()) {
                capture = report.at("capture");
                if (!output_.isEmpty()) {
                    const auto file = QString::fromStdString(capture.get<std::string>());
                    const auto bytes = read(QFileInfo(file).dir().filePath("report.json")).dump(2);
                    QSaveFile saved(output_ + (warp ? "/recapture-warp.json" : "/recapture-hardware.json"));
                    QVERIFY(saved.open(QIODevice::WriteOnly));
                    QCOMPARE(saved.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
                    QVERIFY(saved.commit());
                }
            } else
                QCOMPARE(report.at("capture"), capture);
            QCOMPARE(report.at("gpa_event"), Json(QString(stage) == "cs" ? 51 : 105));
            QVERIFY(!report.at("steps").empty());
            if (!output_.isEmpty()) {
                const auto name = QString(stage) + (warp ? "-warp" : "-hardware");
                view->exportResult(output_ + "/" + name + ".json");
                view->findChild<QTabWidget *>("debugCodeTabs")->setCurrentIndex(1);
                view->findChild<QTabWidget *>("debugValueTabs")->setCurrentIndex(1);
                QVERIFY(window.grab().save(output_ + "/" + name + ".png"));
            }
        }
        auto view = window.findChild<ReplayDebugView *>("replayDebug-cs");
        action(*view, "replayDebugRead")->trigger();
        action(*view, "replayDebugCancel")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(!view->model());
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->exportReport().at("capture"), capture);
        QVERIFY2(dialogs.empty(), qPrintable(dialogs.join('\n')));
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
        QVERIFY(!view->model());
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(!tasks.takeLast()[0].toBool());
        tasks.clear();
        QVERIFY(!view->model());
        undo->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->exportReport().at("capture"), capture);
        view->setBackendPath("C:/missing/renderdoc.dll");
        action(*view, "replayDebugRead")->trigger();
        QCOMPARE(tasks.size(), 1);
        QVERIFY(!tasks.takeLast()[0].toBool());
        QVERIFY(!view->model());
        view->setBackendPath("C:/Program Files/RenderDoc/renderdoc.dll");
        action(*view, "replayDebugRead")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        QCOMPARE(view->exportReport().at("capture"), capture);
        action(*view, "replayDebugRead")->trigger();
        select(105);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(!view->model());
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        tasks.clear();
        auto ps = window.findChild<ReplayDebugView *>("replayDebug-ps");
        auto image = window.findChild<ImageView *>("frameOutput");
        QVERIFY(QMetaObject::invokeMethod(image, "pixelSelected", Qt::DirectConnection, Q_ARG(int, 48),
                                          Q_ARG(int, 32), Q_ARG(QColor, QColor(0, 0, 255))));
        QCOMPARE(ps->findChild<QSpinBox *>("debugX")->value(), 48);
        QCOMPARE(ps->findChild<QSpinBox *>("debugY")->value(), 32);
        auto history = window.findChild<PixelHistoryView *>();
        history->findChild<QLineEdit *>("historyResource")->clear();
        history->findChild<QLineEdit *>("historyEvent")->setText("105");
        action(*history, "readPixelHistory")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 120000);
        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
        tasks.clear();
        QTemporaryDir temp;
        const auto historyPath = temp.path() + "/history.json";
        history->exportResult(historyPath);
        QCOMPARE(read(historyPath).at("capture"), capture);
        if (!output_.isEmpty())
            history->exportResult(output_ + (warp ? "/history-warp.json" : "/history-hardware.json"));
    }
};
QTEST_MAIN(ReplayDebugUiTests)
#include "ReplayDebugUiTests.moc"
