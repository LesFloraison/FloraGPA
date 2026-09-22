#include "app/Appearance.h"
#include "app/FrameRangeDialog.h"
#include "app/IntelMetricsView.h"
#include "app/MainWindow.h"
#include "application/MetricAnalysis.h"
#include "application/MetricIterations.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing fixture: " + path.toStdString());
    return Json::parse(f.readAll().toStdString());
}
void save(const QString &path, const Json &v) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write fixture");
    f.write(QByteArray::fromStdString(v.dump(2)));
}
std::shared_ptr<Frame> frame() {
    return std::make_shared<Frame>(
        (qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT") + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame")
            .toStdWString());
}
QString fixture() { return qEnvironmentVariable("FLORA_TEST_UNIFORM_RESULTS"); }
void copyTree(const QString &from, const QString &to) {
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto src = it.next(), dst = to + '/' + QDir(from).relativeFilePath(src);
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(src, dst))
            throw std::runtime_error("Cannot copy fixture");
    }
}
void fileAction(MainWindow &window, const char *name, const QString &path) {
    bool handled = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &window, [&] {
        auto d = window.findChild<QFileDialog *>();
        if (!d || !d->isVisible())
            return;
        timer.stop();
        d->selectFile(path);
        handled = true;
        QMetaObject::invokeMethod(d, "accept", Qt::QueuedConnection);
    });
    timer.start(20);
    window.findChild<QAction *>(name)->trigger();
    QVERIFY(handled);
}
} // namespace
class UniformMetricsUiTests : public QObject {
    Q_OBJECT
  private slots:
    void legacyAndSharedSettings() {
        const std::array<std::string, 7> scopes = {"当前事件", "整帧",       "指定事件",      "区间整体",
                                                   "整帧整体", "帧指标范围", "全部帧指标范围"};
        for (size_t i = 0; i < scopes.size(); ++i) {
            Json ui = {{"hardware_metrics",
                        {{"scope", scopes[i]},
                         {"events", "4, 2"},
                         {"samples", "3"},
                         {"warmup", "0"},
                         {"event_groups", {{"preserve", true}}},
                         {"metric_request",
                          {{"symbols", "GpuTime, EuActive"},
                           {"search", "gpu"},
                           {"publisher_values", true},
                           {"value_view", "原版数值转换"}}}}}};
            IntelMetricsView owner;
            owner.restoreDocument(ui);
            auto cfg = owner.settings();
            QCOMPARE(cfg["sets"]["scope"], Json(i));
            QCOMPARE(cfg["request"]["symbols"], Json("GpuTime, EuActive"));
            QCOMPARE(cfg["request"]["value_view"], Json(1));
            const auto saved = owner.saveDocument(ui);
            QVERIFY(saved["hardware_metrics"]["event_groups"]["preserve"] == true);
            IntelMetricsView restored;
            restored.restoreDocument(saved);
            QVERIFY(restored.settings() == cfg);
        }
        for (const auto &n :
             {Json("bad"), Json(-1), Json(0), Json(101), Json(uint64_t(0x100000001)), Json(true)})
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                     uniformUiSettings({{"hardware_metrics", {{"samples", n}}}}, false));
        IntelMetricsView owner;
        auto raw = owner.findChild<UniformMetricsView *>("uniformMetricsView"),
             request = owner.findChild<UniformMetricsView *>("requestedMetricsView");
        auto scheduled = owner.findChild<ScheduledMetricsView *>();
        auto cfg = raw->settings();
        cfg.update({{"scope", 5}, {"events", "4,2"}, {"samples", 3}, {"warmup", 0}});
        raw->restoreSettings(cfg);
        QCOMPARE(request->settings()["events"], Json("4,2"));
        QCOMPARE(scheduled->settings()["scope"], Json(1));
        auto schedule = scheduled->settings();
        schedule.update({{"scope", 0}, {"samples", 2}});
        scheduled->restoreSettings(schedule);
        QCOMPARE(raw->settings()["scope"], Json(6));
        QCOMPARE(request->settings()["samples"], Json(2));
    }
    void requestScopesAndRangePicker() {
        if (qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT").isEmpty() || fixture().isEmpty())
            QSKIP("Set reference and uniform fixture paths");
        auto f = frame();
        Experiment experiment(*f);
        const auto catalog = read(fixture() + "/experiment-ranges/catalog.json");
        auto settings = uniformUiSettings(Json::object(), false);
        settings["bridge"] = "bridge.dll";
        Id first{};
        for (const auto &[id, e] : f->entries())
            if (e.category == 7 && isDraw(e.type)) {
                first = id;
                break;
            }
        QVERIFY(first);
        for (int scope = 0; scope < 7; ++scope) {
            settings["scope"] = scope;
            settings["events"] = scope == 5   ? "2,0"
                                 : scope == 3 ? std::to_string(first) + "," + std::to_string(first)
                                              : std::to_string(first);
            auto p = prepareUniformRequest(*f, experiment, catalog, settings, false, first);
            QVERIFY(!p["selection"]["selected_events"].is_null());
            auto args = uniformWorkerArguments("capture", "directory", "bridge", p);
            QCOMPARE(args.first(), QString("metric-profile"));
            QVERIFY(args.contains("--ready-file"));
            if (scope == 5)
                QCOMPARE(p["request"]["frame_ranges"], Json::array({0, 2}));
        }
        settings["scope"] = 5;
        settings["events"] = "0,0";
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                 prepareUniformRequest(*f, experiment, catalog, settings, false, first));
        settings["scope"] = 0;
        experiment.setEnabled(*f, first, false);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                 prepareUniformRequest(*f, experiment, catalog, settings, false, first));
        const auto index = buildFrameMetricIndex(*f);
        const auto endpoint = index["ergs"][index["ranges"]["2"][0][2].get<size_t>()].get<Id>();
        FrameRangeDialog dialog(*f, experiment, endpoint, 6, "");
        auto table = dialog.findChild<QTreeWidget *>("frameRanges");
        QCOMPARE(table->topLevelItemCount(), 75);
        QCOMPARE(table->selectedItems().size(), 75);
        dialog.findChild<QAction *>("rangeClear")->trigger();
        QCOMPARE(dialog.selectedIndices(), QString());
        dialog.findChild<QPushButton *>("rangeApply")->click();
        QCOMPARE(dialog.result(), 0);
        QCOMPARE(dialog.findChild<QLabel *>("rangeStatus")->text(), QString("Select at least one range"));
        dialog.findChild<QAction *>("rangeCurrent")->trigger();
        QCOMPARE(dialog.selectedIndices(), QString("0"));
        QCOMPARE(table->selectedItems()[0]->text(1), QString::number(endpoint));
        dialog.findChild<QPushButton *>("rangeApply")->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        FrameRangeDialog chosen(*f, experiment, first, 5, "2,0");
        QCOMPARE(chosen.selectedIndices(), QString("0, 2"));
        const auto out = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!out.isEmpty()) {
            QDir().mkpath(out);
            chosen.show();
            QTest::qWait(40);
            chosen.grab().save(out + "/frame-ranges.png");
        }
    }
    void frozenResultAndExactDisplay() {
        try {
            if (qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT").isEmpty() || fixture().isEmpty())
                QSKIP("Set reference and uniform fixture paths");
            auto f = frame();
            Experiment experiment(*f);
            const auto source = fixture() + "/experiment-ranges";
            experiment.load(source + "/experiment.json", *f);
            auto profile = read(source + "/profile.json");
            auto cfg = uniformUiSettings(Json::object(), false);
            cfg.update({{"scope", 5},
                        {"events", "0,2"},
                        {"samples", profile["sample_count"]},
                        {"warmup", profile["warmup_count"]},
                        {"publisher_values", true}});
            const auto catalog = read(source + "/catalog.json");
            auto prepared = prepareUniformRequest(*f, experiment, catalog, cfg, false, 0);
            auto dir = std::make_unique<QTemporaryDir>();
            copyTree(source, dir->filePath("result"));
            const auto accepted =
                acceptUniformResult(dir->filePath("result"), prepared, source + "/experiment.json", "frozen");
            QVERIFY(accepted.contains("publisher_result"));
            QVERIFY(!read(dir->filePath("result/profile.json")).contains("publisher_result"));
            for (const auto key : {"frame_sha256", "sets", "plan"}) {
                auto bad = prepared;
                bad[key] = "bad";
                QVERIFY_THROWS_EXCEPTION(
                    std::exception,
                    acceptUniformResult(dir->filePath("result"), bad, source + "/experiment.json", "wrong"));
            }
            QVERIFY(read(dir->filePath("result/profile.json"))["experiment_key"] == "frozen");
            UniformMetricsView view(false);
            view.setContext(f, &experiment, "frozen", 113);
            view.setCatalog(catalog, view.bridgePath());
            cfg["bridge"] = view.bridgePath().toStdString();
            view.restoreSettings(cfg);
            QSignalSpy request(&view, &UniformMetricsView::readRequested);
            view.findChild<QAction *>("uniformMeasure")->trigger();
            QCOMPARE(request.size(), 1);
            auto exact = accepted;
            exact.erase("publisher_result");
            exact["records"][0]["available"] = true;
            exact["records"][0]["values"][0]["value"] = UINT64_MAX;
            exact["records"][0]["values"][0]["type"] = 1;
            QVERIFY(view.finish(request[0][1].toULongLong(), exact, std::move(dir)));
            auto values = view.findChild<QTreeWidget *>("uniformValues");
            QCOMPARE(values->topLevelItem(0)->text(1), QString("18446744073709551615"));
            view.findChild<QComboBox *>("uniformValueView")->setCurrentIndex(1);
            QCOMPARE(values->topLevelItemCount(), 0);
            view.findChild<QComboBox *>("uniformValueView")->setCurrentIndex(0);
            QCOMPARE(values->topLevelItem(0)->text(1), QString("18446744073709551615"));
            view.setContext(f, &experiment, "different", 113);
            QCOMPARE(view.findChild<QLabel *>("uniformSummary")->text(), QString("Previous experiment"));
            QVERIFY(view.result() == exact);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void mainWindowCollectionAndExport() {
        const auto root = qEnvironmentVariable("FLORA_TEST_REFERENCE_ROOT");
        if (root.isEmpty() || qEnvironmentVariableIntValue("FLORA_TEST_INTEL_METRICS") != 1)
            QSKIP("Set Intel opt-in and reference path");
        QTemporaryDir temp;
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy tasks(&window, &MainWindow::taskFinished);
        window.openCapture(root + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !window.capturePath().isEmpty(), 60000);
        auto owner = window.findChild<IntelMetricsView *>();
        auto tabs = qobject_cast<QTabWidget *>(owner->parentWidget()->parentWidget());
        QVERIFY(tabs);
        tabs->setCurrentWidget(owner);
        auto raw = owner->findChild<UniformMetricsView *>("uniformMetricsView"),
             request = owner->findChild<UniformMetricsView *>("requestedMetricsView");
        tasks.clear();
        raw->findChild<QAction *>("uniformReadCatalog")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(tasks.last()[0].toBool());
        QVERIFY(request->findChild<QTreeWidget *>("uniformAvailable")->topLevelItemCount() > 0);
        QVERIFY(owner->findChild<QTreeWidget *>("scheduledAvailable")->topLevelItemCount() > 0);
        auto cfg = raw->settings();
        cfg.update(
            {{"scope", 5}, {"events", "2,0,1"}, {"samples", 2}, {"warmup", 0}, {"publisher_values", true}});
        raw->restoreSettings(cfg);
        tasks.clear();
        raw->findChild<QAction *>("uniformMeasure")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 90000);
        QVERIFY2(tasks.last()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QCOMPARE(raw->result()["records"].size(), size_t(6));
        const auto rawResult = raw->result();
        auto rawValues = raw->findChild<QTreeWidget *>("uniformValues");
        QVERIFY(rawValues->topLevelItemCount() > 0);
        raw->findChild<QComboBox *>("uniformValueView")->setCurrentIndex(1);
        QCOMPARE(
            rawValues->topLevelItem(0)->text(1),
            QString::fromStdString(rawResult["publisher_result"]["records"][0]["values"][0]["value"].dump()));
        owner->findChild<QComboBox *>("intelMetricMode")->setCurrentIndex(1);
        cfg = request->settings();
        cfg.update({{"symbols", "GpuTime, EuActive, Sampler00InputAvailable, Sampler00OutputReady"},
                    {"publisher_values", true}});
        request->restoreSettings(cfg);
        request->findChild<QAction *>("uniformPreview")->trigger();
        QCOMPARE(request->findChild<QTreeWidget *>("uniformPlan")->topLevelItemCount(), 2);
        tasks.clear();
        request->findChild<QAction *>("uniformMeasure")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 90000);
        QVERIFY2(tasks.last()[0].toBool(),
                 qPrintable(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText()));
        QCOMPARE(request->result()["records"].size(), size_t(12));
        QVERIFY(raw->result() == rawResult);
        const auto original = request->result();
        const auto expected = requestedMetricResults(original, original["metric_request"]);
        QCOMPARE(expected["samples"].size(), size_t(24));
        auto records = request->findChild<QComboBox *>("uniformRecords");
        auto values = request->findChild<QTreeWidget *>("uniformValues");
        size_t count = 0;
        for (int i = 0; i < records->count(); ++i) {
            records->setCurrentIndex(i);
            for (int j = 0; j < values->topLevelItemCount(); ++j) {
                auto item = values->topLevelItem(j);
                bool found = false;
                for (const auto &r : expected["samples"]) {
                    const auto &source = original["records"][size_t(i)];
                    if (r["metric"] == item->text(0).toStdString() && r["set"] == source["set"] &&
                        r["sample_index"] == source["sample_index"] &&
                        r["start_event"] == source["start_event"]) {
                        QCOMPARE(item->text(1), r["available"] == true
                                                    ? QString::fromStdString(r["value"].dump())
                                                    : QString("NA"));
                        found = true;
                        break;
                    }
                }
                QVERIFY(found);
                ++count;
            }
        }
        QCOMPARE(count, size_t(24));
        records->setCurrentIndex(0);
        request->findChild<QComboBox *>("uniformValueView")->setCurrentIndex(1);
        const auto converted = publisherMetricAnalysis(original, original["publisher_result"])["requested"];
        QCOMPARE(values->topLevelItem(0)->text(1),
                 QString::fromStdString(converted["samples"][0]["value"].dump()));
        const auto detail =
            Json::parse(request->findChild<QPlainTextEdit *>("uniformDetails")->toPlainText().toStdString());
        QVERIFY(detail["statistics"] == converted["statistics"][0]);
        request->findChild<QComboBox *>("uniformValueView")->setCurrentIndex(0);
        const auto evidence = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            request->exportResult(evidence + "/uniform-request-multipass.zip");
            save(evidence + "/request-multipass-result.json", request->result());
        }
        QSignalSpy navigate(request, &UniformMetricsView::eventRequested);
        request->findChild<QAction *>("uniformLocateEnd")->trigger();
        QCOMPARE(navigate.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        request->exportResult(temp.filePath("request.zip"));
        QFile zip(temp.filePath("request.zip"));
        QVERIFY(zip.open(QIODevice::ReadOnly));
        auto bytes = zip.readAll();
        for (const auto n : {"profile.json", "publisher-values.json", "requested-metrics.json",
                             "process-tree.json", "worker.log"})
            QVERIFY(bytes.contains(n));
        cfg["symbols"] = "GpuTime";
        request->restoreSettings(cfg);
        request->findChild<QAction *>("uniformPreview")->trigger();
        QVERIFY(request->result() == original);
        QFile invalid(temp.filePath("invalid.dll"));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("invalid");
        invalid.close();
        request->findChild<QLineEdit *>("uniformBridge")->setText(invalid.fileName());
        tasks.clear();
        request->findChild<QAction *>("uniformReadCatalog")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(!tasks.last()[0].toBool());
        QVERIFY(request->result() == original);
        QVERIFY(raw->result() == rawResult);
        cfg["samples"] = 100;
        cfg["warmup"] = 100;
        request->restoreSettings(cfg);
        tasks.clear();
        request->findChild<QAction *>("uniformMeasure")->trigger();
        QTRY_VERIFY(window.busy());
        request->findChild<QAction *>("uniformCancel")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(!tasks.last()[0].toBool());
        QVERIFY(request->result() == original);
        cfg["samples"] = 1;
        cfg["warmup"] = 0;
        request->restoreSettings(cfg);
        tasks.clear();
        request->findChild<QAction *>("uniformMeasure")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 90000);
        QVERIFY(tasks.last()[0].toBool());
        auto project = temp.filePath("settings.json");
        fileAction(window, "saveExperiment", project);
        QVERIFY(read(project)["ui"]["intel_metrics"] == owner->settings());
        const auto latest = request->result();
        auto source = frame();
        Experiment edited(*source);
        edited.setEnabled(*source, 113, false);
        edited.save(temp.filePath("changed.json"));
        tasks.clear();
        fileAction(window, "openExperiment", temp.filePath("changed.json"));
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(tasks.last()[0].toBool());
        QCOMPARE(request->findChild<QLabel *>("uniformSummary")->text(), QString("Previous experiment"));
        QVERIFY(request->result() == latest);
        tasks.clear();
        fileAction(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !tasks.empty(), 30000);
        QVERIFY(tasks.last()[0].toBool());
        QVERIFY(read(project)["ui"]["intel_metrics"] == owner->settings());
        const auto out = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!out.isEmpty()) {
            QDir().mkpath(out);
            window.findChild<QDockWidget *>("logDock")->hide();
            request->findChild<QTabWidget *>("uniformTabs")->setCurrentIndex(0);
            QTest::qWait(60);
            window.grab().save(out + "/uniform-request.png");
            request->findChild<QTabWidget *>("uniformTabs")->setCurrentIndex(1);
            QTest::qWait(30);
            window.grab().save(out + "/uniform-setup.png");
            request->findChild<QAction *>("uniformPreview")->trigger();
            QTest::qWait(30);
            window.grab().save(out + "/uniform-plan.png");
            request->exportResult(out + "/uniform-request.zip");
            save(out + "/request-result.json", request->result());
            raw->exportResult(out + "/uniform-sets.zip");
            save(out + "/sets-result.json", raw->result());
        }
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("FloraGPA.Tests");
    app.setApplicationName("UniformMetricsUi");
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
    UniformMetricsUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "UniformMetricsUiTests.moc"
