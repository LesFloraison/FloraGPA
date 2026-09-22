#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/ScheduledMetricsView.h"
#include "application/MdIterationSession.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing test artifact");
    return Json::parse(file.readAll().toStdString());
}
void save(const QString &path, const Json &value) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save test artifact");
    f.write(QByteArray::fromStdString(value.dump(2)));
}
void fileAction(MainWindow &window, const char *name, const QString &path) {
    bool handled = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        auto dialog = window.findChild<QFileDialog *>();
        if (!dialog || !dialog->isVisible())
            return;
        timer.stop();
        dialog->selectFile(path);
        handled = true;
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    timer.start(20);
    window.findChild<QAction *>(name)->trigger();
    QVERIFY(handled);
}
} // namespace
class ScheduledMetricsUiTests : public QObject {
    Q_OBJECT
  private slots:
    void settingsRoundTrip() {
        ScheduledMetricsView view;
        const Json settings = {{"symbols", "GpuTime, EuActive"},
                               {"selected_pass", "1"},
                               {"mapping", "1, 0"},
                               {"weight_path", "weights.json"},
                               {"ranges", "4, 2"},
                               {"scope", 1},
                               {"samples", 3},
                               {"warmup", 2},
                               {"bridge", "bridge.dll"}};
        view.restoreSettings(settings);
        QVERIFY(view.settings() == settings);
        QVERIFY(!view.findChild<QAction *>("scheduledMeasure")->isEnabled());
        QVERIFY(!view.findChild<QPlainTextEdit *>("scheduledDetails")->isVisible());
    }
    void frozenRequestDomain() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT"),
                   results = qEnvironmentVariable("FLORA_TEST_SCHEDULED_RESULTS");
        if (root.isEmpty() || results.isEmpty())
            QSKIP("Set reference and saved collection paths");
        const Frame frame((root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
        Experiment experiment(frame);
        const auto catalog = read(results + "/gf2-experiment/catalog.json");
        const Json base = {{"symbols", {"GpuTime", "EuActive"}},
                           {"frame_ranges", {2, 0, 1}},
                           {"samples", 2},
                           {"warmup", 0},
                           {"requested_pass", 0},
                           {"weights", {1., 2., 3.}}};
        const auto prepared = prepareScheduledRequest(frame, experiment, catalog, base);
        QVERIFY(prepared["request"]["frame_ranges"] == Json::array({0, 1, 2}));
        for (auto bad : {Json{{"samples", 0}}, Json{{"requested_pass", 99}}, Json{{"weights", {1.}}},
                         Json{{"frame_ranges", {0, 0}}}}) {
            auto request = base;
            request.update(bad);
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     prepareScheduledRequest(frame, experiment, catalog, request));
        }
        QTemporaryDir dir;
        const auto args = scheduledWorkerArguments("capture.gpa_frame", dir.path(), "bridge.dll", prepared);
        QVERIFY(args.contains("--ready-file"));
        QVERIFY(args.contains("--weights"));
        QVERIFY(read(dir.filePath("input-weights.json")) == base["weights"]);
    }
    void legacySettings() {
        Json ui = {
            {"hardware_metrics",
             {{"scope", "帧指标范围"},
              {"events", "2,0"},
              {"samples", "3"},
              {"warmup", "0"},
              {"sets", {"RenderBasic"}},
              {"metric_iterations",
               {{"symbols", "GpuTime"}, {"selected_pass", "全部"}, {"mapping", ""}, {"weight_path", ""}}}}}};
        ScheduledMetricsView view;
        view.restoreSettings(scheduledUiSettings(ui));
        QCOMPARE(view.settings()["scope"], Json(1));
        QCOMPARE(view.settings()["samples"], Json(3));
        const auto saved = scheduledUiDocument(ui, view.settings());
        QVERIFY(saved["hardware_metrics"]["sets"] == ui["hardware_metrics"]["sets"]);
        QVERIFY(scheduledUiSettings(saved) == view.settings());
        ui["hardware_metrics"]["scope"] = "当前事件";
        QCOMPARE(scheduledUiSettings(ui)["scope"], Json(-1));
        ui["hardware_metrics"]["samples"] = "invalid";
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, scheduledUiSettings(ui));
        ui["hardware_metrics"]["samples"] = uint64_t(0x100000001);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, scheduledUiSettings(ui));
    }
    void frozenResultAcceptance() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT"),
                   results = qEnvironmentVariable("FLORA_TEST_SCHEDULED_RESULTS");
        if (root.isEmpty() || results.isEmpty())
            QSKIP("Set reference and saved collection paths");
        const QString source = results + "/gf2-experiment";
        const Frame frame((root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
        Experiment experiment(frame);
        experiment.load(source + "/experiment.json", frame);
        const auto profile = read(source + "/scheduled-profile.json");
        Json indices = Json::array();
        for (const auto &r : profile["selection"]["ranges"])
            indices.push_back(r["range_index"]);
        const auto prepared = prepareScheduledRequest(frame, experiment, read(source + "/catalog.json"),
                                                      {{"symbols", profile["requested_metrics"]},
                                                       {"samples", profile["requested_samples"]},
                                                       {"warmup", profile["warmup_count"]},
                                                       {"frame_ranges", indices},
                                                       {"requested_pass", profile["requested_pass"]},
                                                       {"pass_mapping", profile["pass_mapping"]},
                                                       {"weights", profile["supplied_weights"]}});
        QTemporaryDir temp;
        const auto target = temp.filePath("result");
        QDirIterator it(source, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto file = it.next();
            const auto destination = target + '/' + QDir(source).relativeFilePath(file);
            QDir().mkpath(QFileInfo(destination).absolutePath());
            QVERIFY(QFile::copy(file, destination));
        }
        const auto accepted =
            acceptScheduledResult(target, prepared, source + "/experiment.json", "frozen-key");
        QCOMPARE(accepted["experiment_key"], Json("frozen-key"));
        QVERIFY(accepted.contains("publisher_result"));
        QVERIFY(!read(target + "/scheduled-profile.json").contains("publisher_result"));
        for (const auto key : {"samples", "warmup", "requested_pass", "symbols", "weights"}) {
            auto bad = prepared;
            bad["request"][key] = nullptr;
            QVERIFY_THROWS_EXCEPTION(
                std::invalid_argument,
                acceptScheduledResult(target, bad, source + "/experiment.json", "wrong-key"));
        }
        auto bad = prepared;
        bad["frame_sha256"] = "wrong";
        QVERIFY_THROWS_EXCEPTION(
            std::invalid_argument,
            acceptScheduledResult(target, bad, source + "/experiment.json", "wrong-key"));
        QVERIFY(read(target + "/scheduled-profile.json")["experiment_key"] == "frozen-key");
    }
    void mainWindowCollectionAndExport() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
        if (root.isEmpty() || qEnvironmentVariableIntValue("FLORA_TEST_INTEL_METRICS") != 1)
            QSKIP("Set Intel metric opt-in and capture path");
        QTemporaryDir dir;
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !window.capturePath().isEmpty(), 60000);
        auto view = window.findChild<ScheduledMetricsView *>();
        QVERIFY(view);
        auto tabs = qobject_cast<QTabWidget *>(view->parentWidget()->parentWidget());
        QVERIFY(tabs);
        tabs->setCurrentWidget(view);
        tasks.clear();
        view->findChild<QAction *>("scheduledReadCatalog")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY2(tasks.last()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QVERIFY(view->findChild<QTreeWidget *>("scheduledAvailable")->topLevelItemCount() > 0);
        auto available = view->findChild<QTreeWidget *>("scheduledAvailable");
        available->setCurrentItem(available->topLevelItem(0));
        view->findChild<QAction *>("scheduledUseMetrics")->trigger();
        QCOMPARE(view->findChild<QLineEdit *>("scheduledSymbols")->text(),
                 available->topLevelItem(0)->text(0));
        auto settings = view->settings();
        settings.update({{"symbols", "GpuTime, EuActive, Sampler00InputAvailable, Sampler00OutputReady"},
                         {"scope", 1},
                         {"ranges", "2,0,1"},
                         {"warmup", 0}});
        view->restoreSettings(settings);
        view->findChild<QAction *>("scheduledPreview")->trigger();
        QCOMPARE(view->findChild<QTreeWidget *>("scheduledPlan")->topLevelItemCount(), 2);
        tasks.clear();
        view->findChild<QAction *>("scheduledMeasure")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 90000);
        QVERIFY2(tasks.last()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        const auto original = view->result();
        QCOMPARE(original["records"].size(), size_t(9));
        QCOMPARE(original["metrics"].size(), size_t(12));
        auto table = view->findChild<QTreeWidget *>("scheduledValues");
        QCOMPARE(table->topLevelItemCount(), 3);
        QVERIFY(table->currentItem());
        QSignalSpy navigate(view, &ScheduledMetricsView::eventRequested);
        view->findChild<QAction *>("scheduledLocateEnd")->trigger();
        QCOMPARE(navigate.size(), 1);
        QCOMPARE(navigate[0][0].toULongLong(), original["metrics"][0]["end_event"].get<qulonglong>());
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        const auto archive = dir.filePath("scheduled.zip");
        view->exportResult(archive);
        QFile zip(archive);
        QVERIFY(zip.open(QIODevice::ReadOnly));
        const auto bytes = zip.readAll();
        for (const auto name : {"scheduled-profile.json", "publisher-values.json", "scheduler-audit.json",
                                "process-tree.json", "worker.log", "range-0.bin"})
            QVERIFY(bytes.contains(name));
        settings["selected_pass"] = "0";
        settings["mapping"] = "1, 0";
        settings["samples"] = 4;
        view->restoreSettings(settings);
        view->findChild<QAction *>("scheduledPreview")->trigger();
        QVERIFY(view->result() == original);
        tasks.clear();
        view->findChild<QAction *>("scheduledMeasure")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 90000);
        QVERIFY(tasks.last()[0].toBool());
        QCOMPARE(view->result()["actual_iteration_count"], Json(1));
        QVERIFY(std::any_of(view->result()["metrics"].begin(), view->result()["metrics"].end(),
                            [](const Json &c) { return c["measured"] == false; }));
        const auto prior = view->result();
        QFile invalidBridge(dir.filePath("invalid.dll"));
        QVERIFY(invalidBridge.open(QIODevice::WriteOnly));
        invalidBridge.write("invalid");
        invalidBridge.close();
        view->findChild<QLineEdit *>("scheduledBridge")->setText(invalidBridge.fileName());
        tasks.clear();
        view->findChild<QAction *>("scheduledReadCatalog")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(!tasks.last()[0].toBool());
        QVERIFY(view->result() == prior);
        settings["mapping"] = "";
        settings["selected_pass"] = "0";
        settings["samples"] = 100;
        settings["warmup"] = 100;
        view->restoreSettings(settings);
        tasks.clear();
        view->findChild<QAction *>("scheduledMeasure")->trigger();
        QTRY_VERIFY(window.busy());
        view->findChild<QAction *>("scheduledCancel")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(!tasks.last()[0].toBool());
        QVERIFY(view->result() == prior);
        settings["samples"] = 1;
        settings["warmup"] = 0;
        save(dir.filePath("weights.json"), Json::array({0., 2., 4.}));
        settings["weight_path"] = dir.filePath("weights.json").toStdString();
        view->restoreSettings(settings);
        tasks.clear();
        view->findChild<QAction *>("scheduledMeasure")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 90000);
        QVERIFY(tasks.last()[0].toBool());
        QCOMPARE(view->result()["weight_source"], Json("caller_cache"));
        QCOMPARE(view->result()["replays"].size(), size_t(1));
        const auto project = dir.filePath("settings.json");
        fileAction(window, "saveExperiment", project);
        QVERIFY(read(project)["ui"]["scheduled_metrics"] == view->settings());
        const auto latest = view->result();
        const Frame sourceFrame((root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame").toStdWString());
        Experiment changed(sourceFrame);
        Id draw{};
        for (const auto &[id, e] : sourceFrame.entries())
            if (e.category == 7 && e.type >= 0x35 && e.type <= 0x3d) {
                draw = id;
                break;
            }
        QVERIFY(draw);
        changed.setEnabled(sourceFrame, draw, false);
        changed.save(dir.filePath("changed.json"));
        tasks.clear();
        fileAction(window, "openExperiment", dir.filePath("changed.json"));
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(tasks.last()[0].toBool());
        QCOMPARE(view->findChild<QLabel *>("scheduledSummary")->text(), QString("Previous experiment"));
        QVERIFY(view->result() == latest);
        tasks.clear();
        fileAction(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(tasks.last()[0].toBool());
        QVERIFY(view->settings() == read(project)["ui"]["scheduled_metrics"]);
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            window.findChild<QDockWidget *>("logDock")->hide();
            view->findChild<QTabWidget *>("scheduledTabs")->setCurrentIndex(0);
            QTest::qWait(100);
            window.grab().save(artifact + "/scheduled-metrics.png");
            view->findChild<QTabWidget *>("scheduledTabs")->setCurrentIndex(1);
            QTest::qWait(30);
            window.grab().save(artifact + "/scheduled-setup.png");
            view->findChild<QAction *>("scheduledPreview")->trigger();
            QTest::qWait(30);
            window.grab().save(artifact + "/scheduled-plan.png");
            view->exportResult(artifact + "/scheduled-metrics.zip");
            save(artifact + "/ui-result.json", view->result());
        }
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("FloraGPA.Tests");
    app.setApplicationName("ScheduledMetricsUi");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    applyAppearance(app);
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto box = qobject_cast<QMessageBox *>(widget))
                box->accept();
    });
    dismiss.start(20);
    ScheduledMetricsUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ScheduledMetricsUiTests.moc"
