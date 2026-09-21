#include "HullCapture.h"
#include "StreamCapture.h"
#include "app/Appearance.h"
#include "app/CheckpointView.h"
#include "app/MainWindow.h"
#include "app/NativeDebugControls.h"
#include "application/ZipArchive.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read test report");
    const auto bytes = file.readAll();
    return Json::parse(bytes.begin(), bytes.end());
}
void write(const QString &path, const Json &value) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save test report");
    const auto bytes = value.dump(2);
    if (file.write(bytes.data(), qsizetype(bytes.size())) != qsizetype(bytes.size()))
        throw std::runtime_error("Cannot save test report");
}
} // namespace
class CheckpointUiTests final : public QObject {
    Q_OBJECT
    QString output_;
    void snapshot(QWidget &widget, const QString &name) {
        if (!output_.isEmpty())
            QVERIFY(widget.grab().save(output_ + '/' + name + ".png"));
    }
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setOrganizationName("FloraGPA");
        QCoreApplication::setApplicationName("FloraGPA-CheckpointTests");
        QSettings().clear();
        applyAppearance(*qApp);
        output_ = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!output_.isEmpty())
            QVERIFY(QDir().mkpath(output_));
    }
    void archiveTransactions() {
        QTemporaryDir directory;
        const auto path = directory.path() + "/archive.zip";
        writeZipArchive(path,
                        {{"empty", {}, {}}, {QString::fromUtf8("源.txt"), {}, QByteArray("abc\0def", 7)}});
        QFile before(path);
        QVERIFY(before.open(QIODevice::ReadOnly));
        const auto original = before.readAll();
        before.close();
        QVERIFY(original.startsWith("PK"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 writeZipArchive(path, {{"x", {}, "first"}, {"x", {}, "duplicate"}}));
        QVERIFY(before.open(QIODevice::ReadOnly));
        QCOMPARE(before.readAll(), original);
        before.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeZipArchive(path, {{"../escape", {}, "bad"}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 writeZipArchive(path, {{"missing", directory.path() + "/missing", {}}}));
        QVERIFY(before.open(QIODevice::ReadOnly));
        QCOMPARE(before.readAll(), original);
        if (!output_.isEmpty()) {
            before.close();
            QVERIFY(QFile::copy(path, output_ + "/archive-transaction.zip"));
        }
    }
    void nativeWorker_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("warp");
        for (const QString stage : {"gs", "hs", "ds"})
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(stage + (warp ? "-warp" : "-hardware"))) << stage << warp;
    }
    void nativeWorker() {
        QFETCH(QString, stage);
        QFETCH(bool, warp);
        try {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto path = directory.path() + "/fixture.gpa_frame";
            const auto event = stage == "gs" ? 150ull : 200ull;
            if (stage == "gs")
                testing::streamCapture().save(path);
            else
                testing::hullCapture().save(path);
            MainWindow window;
            window.resize(1500, 950);
            window.show();
            QStringList dialogs;
            QTimer closeDialogs;
            connect(&closeDialogs, &QTimer::timeout, [&] {
                for (auto widget : QApplication::topLevelWidgets())
                    if (auto box = qobject_cast<QMessageBox *>(widget)) {
                        dialogs << box->text();
                        box->reject();
                    }
            });
            closeDialogs.start(25);
            QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
            window.openCapture(path);
            QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
            auto adapter = window.findChild<QComboBox *>("replayAdapter");
            QVERIFY(adapter);
            if (adapter->currentIndex() != int(warp)) {
                adapter->setCurrentIndex(int(warp));
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
            }
            auto api = window.findChild<QTableView *>("apiLog");
            QVERIFY(api);
            for (int i = 0; i < api->model()->rowCount(); ++i)
                if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == event) {
                    api->setCurrentIndex(api->model()->index(i, 0));
                    break;
                }
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
            auto stages = window.findChild<QTabWidget *>("checkpointStageTabs");
            QVERIFY(stages);
            auto view = window.findChild<CheckpointView *>("checkpoint-" + stage);
            QVERIFY(view);
            window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(stages);
            stages->setCurrentWidget(view);
            view->findChild<QAction *>("checkpointRead")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
            tasks.clear();
            auto instructions = view->findChild<QTreeWidget *>("checkpointInstructions");
            QVERIFY(instructions->topLevelItemCount() > 0);
            view->findChild<QAction *>("checkpointBreakpoint")->trigger();
            QCOMPARE(view->controls()->settings().instructions.size(), size_t(1));
            view->findChild<QAction *>("checkpointCapture")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
            tasks.clear();
            auto hits = view->findChild<QTreeWidget *>("checkpointHits");
            QVERIFY(hits->topLevelItemCount() > 0);
            QVERIFY(!view->exportReport().value("trace", false));
            QVERIFY(view->findChild<QTreeWidget *>("checkpointRegisters")->topLevelItemCount() > 0);
            view->findChild<QAction *>("checkpointTrace")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
            tasks.clear();
            QVERIFY(view->exportReport().at("trace").get<bool>());
            QCOMPARE(view->controls()->settings().instructions.size(), size_t(1));
            view->findChild<QAction *>("checkpointStep")->trigger();
            QVERIFY(hits->currentItem() == hits->topLevelItem(1));
            view->findChild<QAction *>("checkpointPrevious")->trigger();
            QVERIFY(hits->currentItem() == hits->topLevelItem(0));
            view->findChild<QComboBox *>("checkpointMatch")->setCurrentIndex(1);
            view->findChild<QAction *>("checkpointSelected")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
            QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
            tasks.clear();
            QVERIFY(view->exportReport().at("register_capture").contains("input_selector"));
            if (stage == "hs") {
                const auto report = view->exportReport();
                const auto phase = report.at("register_capture").at("hs_phase").at("id");
                for (const auto &entry : report.at("catalog"))
                    if (entry.at("checkpoint_allowed") == true && entry.at("hs_phase") != phase) {
                        view->findChild<QLineEdit *>("checkpointInstruction")
                            ->setText(QString::number(entry.at("instruction").get<uint64_t>()));
                        view->findChild<QAction *>("checkpointTrace")->trigger();
                        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
                        QVERIFY2(tasks.takeLast()[0].toBool(), qPrintable(dialogs.join('\n')));
                        tasks.clear();
                        QVERIFY(view->exportReport().at("register_capture").at("hs_phase").at("id") ==
                                entry.at("hs_phase"));
                        break;
                    }
            }
            const auto label = stage + (warp ? "-warp" : "-hardware");
            snapshot(window, "checkpoint-" + label);
            if (!output_.isEmpty())
                view->exportArchive(output_ + "/checkpoint-" + label + ".zip");
            QVERIFY2(dialogs.empty(), qPrintable(dialogs.join('\n')));
            if (stage == "gs" && !warp) {
                const auto saved = view->exportReport();
                QAction *cancel = nullptr, *disable = nullptr, *undo = nullptr;
                for (auto action : window.findChildren<QAction *>()) {
                    if (action->shortcut() == QKeySequence("Escape"))
                        cancel = action;
                    if (action->text() == "Disable Event")
                        disable = action;
                    if (action->shortcut() == QKeySequence::Undo)
                        undo = action;
                }
                QVERIFY(cancel && disable && undo);
                view->findChild<QAction *>("checkpointTrace")->trigger();
                QVERIFY(window.busy());
                cancel->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(!tasks.takeLast()[0].toBool());
                tasks.clear();
                QVERIFY(view->exportReport() == saved);
                // A subsequent capture still works after cancellation.
                view->findChild<QAction *>("checkpointTrace")->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                const auto afterCancel = view->exportReport();
                QSignalSpy stale(view, &CheckpointView::error);
                api->setCurrentIndex(api->model()->index(0, 0));
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                view->findChild<QAction *>("checkpointTrace")->trigger();
                QCOMPARE(stale.count(), 1);
                QVERIFY(view->exportReport() == afterCancel);
                for (int i = 0; i < api->model()->rowCount(); ++i)
                    if (api->model()->index(i, 0).data(Qt::UserRole).toULongLong() == event)
                        api->setCurrentIndex(api->model()->index(i, 0));
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                disable->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                view->findChild<QAction *>("checkpointTrace")->trigger();
                QCOMPARE(stale.count(), 2);
                undo->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                view->findChild<QAction *>("checkpointTrace")->trigger();
                QCOMPARE(stale.count(), 3);
                view->findChild<QAction *>("checkpointRead")->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                view->findChild<QAction *>("checkpointTrace")->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 60000);
                QVERIFY(tasks.takeLast()[0].toBool());
                tasks.clear();
                QCOMPARE(stale.count(), 3);
            }
            // Real main-window context changes must leave the existing snapshot
            // inspectable while refusing to submit its old shader/input selection.
            const auto old = view->exportReport();
            adapter->setCurrentIndex(1 - int(warp));
            QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
            QVERIFY(tasks.takeLast()[0].toBool());
            tasks.clear();
            QSignalSpy rejected(view, &CheckpointView::error);
            view->findChild<QAction *>("checkpointTrace")->trigger();
            QCOMPARE(rejected.count(), 1);
            QVERIFY(view->exportReport() == old);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void recoveredCaptures() {
        try {
            const auto root = qEnvironmentVariable("FLORA_CHECKPOINT_FIXTURES");
            if (root.isEmpty())
                QSKIP("Set FLORA_CHECKPOINT_FIXTURES for original SPDB/SDBG capture view checks");
            Json checks = Json::array(), navigationChecks = Json::array();
            QStringList directories;
            for (const auto &part : root.split(';', Qt::SkipEmptyParts))
                for (const auto &folder :
                     QDir(part).entryList({"*-trace-native"}, QDir::Dirs | QDir::NoDotAndDotDot))
                    directories << part + '/' + folder;
            QVERIFY(!directories.isEmpty());
            for (const auto &directory : directories) {
                const auto folder = QFileInfo(directory).fileName();
                const auto report = read(directory + "/checkpoint.json");
                const auto stage = QString::fromStdString(report.at("register_capture").at("shader_stage"));
                CheckpointView view(stage);
                view.resize(1180, 740);
                view.show();
                view.setContext("fixture", true);
                QSignalSpy errors(&view, &CheckpointView::error),
                    requests(&view, &CheckpointView::captureRequested);
                view.loadOutput(directory, true);
                auto hits = view.findChild<QTreeWidget *>("checkpointHits");
                QVERIFY(hits->topLevelItemCount() > 0);
                auto controls = view.controls();
                for (const auto *expression : {"1u + 2u", "value", "wide", "a[0].x"})
                    controls->settings().watches.emplace_back(expression);
                const auto count = hits->topLevelItemCount();
                for (const auto index : {count - 1, count / 2, 0}) {
                    hits->setCurrentItem(hits->topLevelItem(index));
                    const auto snapshot = view.exportReport();
                    checks.push_back(
                        {{"capture", directory.toStdString()},
                         {"index", index},
                         {"frame", nullptr},
                         {"variables", snapshot.value("selected_source_variables", Json(nullptr))},
                         {"watches", snapshot.at("selected_native_watches")}});
                    auto frames = view.findChild<QComboBox *>("checkpointSourceFrame");
                    for (int frame = 1; frame < frames->count(); ++frame) {
                        frames->setCurrentIndex(frame);
                        const auto state = view.exportReport();
                        checks.push_back(
                            {{"capture", directory.toStdString()},
                             {"index", index},
                             {"frame", state.at("selected_source_frame")},
                             {"variables", state.value("selected_source_variables", Json(nullptr))},
                             {"watches", state.at("selected_native_watches")}});
                    }
                    frames->setCurrentIndex(0);
                }
                QVERIFY2(
                    errors.empty(),
                    qPrintable(folder + " " + (errors.empty() ? "" : errors.first().first().toString())));
                // Record actual button outcomes for comparison with the Python
                // controller, including errors at visible invocation boundaries.
                auto navigation = view.findChild<QComboBox *>("checkpointNavigation");
                auto instruction = view.findChild<QLineEdit *>("checkpointInstruction");
                std::set<int> starts{0, count / 2, count - 1};
                for (int i = 0; i < count; ++i)
                    if (controls->settings().source.stack(size_t(i)).at("frames").size() > 1) {
                        starts.insert(i);
                        break;
                    }
                for (int i = 0; i < count; ++i)
                    if (report.at("hits_preview").at(size_t(i)).value("call_depth", 0u) > 0) {
                        starts.insert(i);
                        break;
                    }
                for (int mode : {0, 1}) {
                    navigation->setCurrentIndex(mode);
                    for (int start : starts) {
                        for (const auto *name : {"Previous", "Step", "Over", "Out", "RunTo", "Continue"}) {
                            hits->setCurrentItem(hits->topLevelItem(start));
                            const auto targetInstruction = report.at("hits_preview")
                                                               .at(std::min(start + 2, count - 1))
                                                               .at("instruction")
                                                               .get<uint64_t>();
                            instruction->setText(QString::number(targetInstruction));
                            controls->settings().instructions = {targetInstruction};
                            auto &source = controls->settings().source;
                            if (mode == 1 && source.exportBreakpoints().empty()) {
                                for (size_t j = size_t(start); j < report.at("hits_preview").size(); ++j) {
                                    const auto location = source.location(j);
                                    if (!location.is_null()) {
                                        source.setRule(location.at("file"), location.at("line_start"),
                                                       "1u == 1u", "equal", 2);
                                        break;
                                    }
                                }
                            }
                            errors.clear();
                            view.findChild<QAction *>(QString("checkpoint") + name)->trigger();
                            navigationChecks.push_back(
                                {{"capture", directory.toStdString()},
                                 {"mode", mode},
                                 {"start", start},
                                 {"action", name},
                                 {"instruction", targetInstruction},
                                 {"rules", source.exportBreakpoints()},
                                 {"rejected", !errors.empty()},
                                 {"target", hits->currentItem()->data(0, Qt::UserRole).toULongLong()}});
                            QVERIFY(errors.size() <= 1);
                        }
                    }
                }
                // Reset temporary rules before rendering the exported example.
                view.loadOutput(directory, true);
                errors.clear();
                view.findChild<QComboBox *>("checkpointNavigation")->setCurrentIndex(1);
                size_t target = 0;
                try {
                    target = controls->settings().source.next(0, 1);
                } catch (const std::exception &) {
                    continue;
                }
                view.findChild<QAction *>("checkpointStep")->trigger();
                QCOMPARE(hits->currentItem()->data(0, Qt::UserRole).toULongLong(), qulonglong(target));
                QVERIFY(errors.empty());
                if (folder == "sdbg-array-hardware-trace-native" ||
                    folder == "spdb-nested-5-hardware-trace-native") {
                    view.findChild<QTabWidget *>("checkpointCodeTabs")->setCurrentIndex(1);
                    view.findChild<QTabWidget *>("checkpointValueTabs")->setCurrentIndex(1);
                    snapshot(view, folder);
                    controls->findChild<QLineEdit *>("debugWatchExpression")->setText("1u + 2u");
                    controls->findChild<QAbstractButton *>("debugAddWatch")->click();
                    QCOMPARE(controls->watchResults().at(0).at("text").get<std::string>(), std::string("3"));
                    view.findChild<QTabWidget *>("checkpointValueTabs")->setCurrentIndex(2);
                    snapshot(view, folder + "-watches");
                    if (!output_.isEmpty())
                        view.exportArchive(output_ + '/' + folder + ".zip");
                }
                // Context changes cannot submit stale original inputs.
                view.setContext("different", true);
                view.findChild<QAction *>("checkpointTrace")->trigger();
                QCOMPARE(requests.count(), 0);
                QCOMPARE(errors.count(), 1);
                view.setContext("non-draw", false);
                QVERIFY(view.findChild<QAction *>("checkpointExport")->isEnabled());
                QVERIFY(!view.findChild<QAction *>("checkpointRead")->isEnabled());
            }
            if (!output_.isEmpty()) {
                write(output_ + "/selected-values.json", checks);
                write(output_ + "/navigation.json", navigationChecks);
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
QTEST_MAIN(CheckpointUiTests)
#include "CheckpointUiTests.moc"
