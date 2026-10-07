#include "DepthStencilCapture.h"
#include "app/Appearance.h"
#include "app/GpuProfileView.h"
#include "app/MainWindow.h"
#include "application/GpuProfile.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsRectItem>
#include <QGraphicsView>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
Json load(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open probe");
    return Json::parse(f.readAll().toStdString());
}
void save(const QString &path, const Json &value) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write probe");
    f.write(QByteArray::fromStdString(value.dump(2)));
}
void fileAction(MainWindow &window, const char *name, const QString &path) {
    bool handled = false, timedOut = false;
    unsigned attempts = 0;
    QTimer choose, deadline;
    QObject::connect(&choose, &QTimer::timeout, &window, [&] {
        for (auto dialog : window.findChildren<QFileDialog *>()) {
            if (!dialog->isVisible()) continue;
            // selectFile() may leave an active filename editor unchanged.
            // Enter the path as a user would, as other UI fixtures already do.
            auto filename = dialog->findChild<QLineEdit *>("fileNameEdit");
            QVERIFY(filename);
            filename->setText(path);
            handled = true;
            ++attempts;
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            break;
        }
    });
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &window, [&] {
        timedOut = true;
        for (auto dialog : window.findChildren<QFileDialog *>()) {
            if (!dialog->isVisible()) continue;
            qWarning() << "File dialog timeout" << name << dialog->windowTitle() << dialog->selectedFiles();
            dialog->reject();
        }
    });
    choose.start(20);
    deadline.start(10000);
    window.findChild<QAction *>(name)->trigger();
    choose.stop();
    deadline.stop();
    qInfo() << "File dialog" << name << "attempts" << attempts;
    QVERIFY(handled && !timedOut);
}
Json run(const Json &job) {
    if (job.at("action") == "distribution")
        return timingDistribution(job.at("values").get<std::vector<double>>());
    if (job.at("action") == "timing") {
        auto v = job.at("values");
        return profileTiming(v[0], v[1].get<bool>(), v[2], v[3], v[4], v[5]);
    }
    const auto request = gpuProfileRequest(job.value("request", Json::object()));
    Frame frame(QString::fromStdString(job.at("capture").get<std::string>()).toStdWString());
    auto selected = gpuProfileSelection(frame, request);
    if (job.at("action") == "selection")
        return {{"start", selected.start}, {"end", selected.end}, {"events", selected.events}};
    ReplayOptions options;
    options.warp = job.value("warp", false);
    options.until = selected.end;
    for (auto id : job.value("disabled", Json::array()))
        options.disabled.insert(id.get<Id>());
    if (job.contains("experiment")) {
        Experiment experiment(frame);
        experiment.load(QString::fromStdString(job.at("experiment").get<std::string>()), frame);
        experiment.apply(frame, options);
    }
    Replay replay(frame, options);
    auto result =
        profileGpu(replay, frame, request, QString::fromStdString(job.at("out").get<std::string>()));
    Json storage = Json::object();
    for (auto id : job.value("buffers", Json::array()))
        storage[std::to_string(id.get<Id>())] = sha256(replay.readBuffer(id));
    for (auto id : job.value("textures", Json::array()))
        storage[std::to_string(id.get<Id>())] = sha256(replay.readTexture(id));
    if (job.value("output", false))
        storage["output"] = sha256(replay.output().rgba);
    result["test_storage"] = storage;
    result["test_draw_auto"] = Json::object();
    for (const auto &[id, value] : replay.drawAutoResults())
        result["test_draw_auto"][std::to_string(id)] = value.vertexCount;
    return result;
}
} // namespace
class GpuProfileTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { applyAppearance(*qobject_cast<QApplication *>(QCoreApplication::instance())); }
    void numericAndValidation() {
        QVERIFY(timingDistribution({})["median_ms"].is_null());
        QVERIFY(timingDistribution({2})["sample_stddev_ms"].is_null());
        auto d = timingDistribution({4, 1, 3, 2});
        QCOMPARE(d["mean_ms"], Json(2.5));
        QCOMPARE(d["median_ms"], Json(2.5));
        QVERIFY(std::abs(d["p05_ms"].get<double>() - 1.15) < 1e-12);
        QVERIFY(std::abs(d["sample_stddev_ms"].get<double>() - std::sqrt(5. / 3)) < 1e-12);
        for (auto data : {std::array<uint64_t, 6>{0, 0, 1, 2, 0, 3},
                          {100, 1, 1, 2, 0, 3},
                          {100, 0, 2, 1, 0, 3},
                          {100, 0, 0, 2, 1, 3}}) {
            auto r = profileTiming(data[0], data[1] != 0, data[2], data[3], data[4], data[5]);
            QVERIFY(!r["available"].get<bool>());
            QVERIFY(r["elapsed_ms"].is_null());
        }
        QCOMPARE(profileTiming(1000, false, 10, 15, 5, 20)["elapsed_ms"], Json(5.));
        for (auto bad : {Json{{"samples", 0}}, Json{{"samples", 1001}}, Json{{"samples", true}},
                         Json{{"warmup", -1}}, Json{{"include_writes", 1}}, Json{{"start", 1.5}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, gpuProfileRequest(bad));
    }
    void gpuStorageAndBoundaries() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true})
            for (bool disabled : {false, true}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = 2000;
                if (disabled)
                    options.disabled.insert(2000);
                Replay replay(frame, options);
                replay.run();
                const auto pixels = replay.output().rgba;
                GpuProfileRequest req;
                req.samples = 3;
                req.warmup = 2;
                req.includeWrites = true;
                auto report =
                    profileGpu(replay, frame, req, dir.filePath(QString("%1-%2").arg(warp).arg(disabled)));
                QCOMPARE(replay.generation(), 6ull);
                QCOMPARE(replay.output().rgba, pixels);
                QCOMPARE(report["events"].size(), size_t(4));
                QCOMPARE(report["events"][3]["enabled"], Json(!disabled));
                QCOMPARE(report["events"][0]["boundary"], Json("complete_replay_command"));
                for (const auto &pass : report["passes"]) {
                    uint64_t previous = pass["envelope"]["start_tick"];
                    for (const auto &row : report["events"]) {
                        const auto &v = pass["events"][std::to_string(row["event"].get<Id>())];
                        QVERIFY(v["available"].get<bool>());
                        QVERIFY(v["start_tick"].get<uint64_t>() >= previous);
                        previous = v["end_tick"];
                        QCOMPARE(v,
                                 profileTiming(pass["frequency_hz"], false, v["start_tick"], v["end_tick"],
                                               pass["envelope"]["start_tick"], pass["envelope"]["end_tick"]));
                    }
                }
                for (const auto &row : report["events"])
                    QCOMPARE(row["valid_samples"], Json(3));
                auto invalid = req;
                invalid.start = 810;
                invalid.end = 800;
                const auto generation = replay.generation();
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         profileGpu(replay, frame, invalid, dir.filePath("invalid")));
                QCOMPARE(replay.generation(), generation);
            }
        GpuProfileRequest req;
        req.start = 800;
        req.end = 810;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, gpuProfileSelection(frame, req));
        req.includeWrites = true;
        QCOMPARE(gpuProfileSelection(frame, req).events, (std::vector<Id>{800, 810}));
    }
    void workerUiTimelineAndExport() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto view = window.findChild<GpuProfileView *>();
        QVERIFY(view);
        window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(view);
        view->findChild<QLineEdit *>("profileSamples")->setText("3");
        view->findChild<QLineEdit *>("profileWarmup")->setText("1");
        view->findChild<QCheckBox *>("profileWrites")->setChecked(true);
        const auto settings = view->settings();
        done.clear();
        view->findChild<QAction *>("sampleGpuProfile")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        auto table = view->findChild<QTreeWidget *>("profileEvents");
        QCOMPARE(table->topLevelItemCount(), 4);
        QCOMPARE(view->findChild<QComboBox *>("profilePass")->count(), 3);
        view->findChild<QComboBox *>("profilePass")->setCurrentIndex(2);
        auto timeline = view->findChild<QGraphicsView *>("profileTimeline");
        QCoreApplication::processEvents();
        QGraphicsItem *bar = nullptr;
        for (auto item : timeline->scene()->items())
            if (item->data(0).isValid() &&
                (!bar || item->sceneBoundingRect().width() > bar->sceneBoundingRect().width())) {
                bar = item;
            }
        QVERIFY(bar);
        QSignalSpy navigation(view, &GpuProfileView::eventRequested);
        const auto event = bar->data(0).toULongLong();
        auto point = timeline->mapFromScene(bar->sceneBoundingRect().center());
        // Sub-tick durations can render as a one-pixel bar at a fractional
        // coordinate. Choose a real hit pixel rather than rounding outside it.
        bool hit = false;
        for (int dx : {0, -1, 1})
            if (timeline->itemAt(point + QPoint(dx, 0)) == bar) {
                point += QPoint(dx, 0);
                hit = true;
                break;
            }
        QVERIFY(hit);
        QTest::mouseClick(timeline->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(navigation.size(), 1);
        QCOMPARE(navigation[0][0].toULongLong(), event);
        view->exportResult(dir.filePath("timings.zip"));
        QFile zip(dir.filePath("timings.zip"));
        QVERIFY(zip.open(QIODevice::ReadOnly));
        const auto bytes = zip.readAll();
        for (auto name :
             {"profile.json", "profile.csv", "profile-samples.csv", "result.json", "loaded_modules.json"})
            QVERIFY(bytes.contains(name));
        QCOMPARE(view->settings(), settings);
        const auto artifact = qEnvironmentVariable("FLORA_UI_ARTIFACT_DIR");
        if (!artifact.isEmpty()) {
            QDir().mkpath(artifact);
            window.findChild<QTabWidget *>("analysisTabs")->setCurrentWidget(view);
            QVERIFY(window.grab().save(artifact + "/gpu-profile.png"));
        }
        QSignalSpy requests(view, &GpuProfileView::readRequested);
        view->findChild<QLineEdit *>("profileSamples")->setText("0");
        view->findChild<QAction *>("sampleGpuProfile")->trigger();
        QCOMPARE(requests.size(), 0);
        view->restoreSettings(settings);
        const auto project = dir.filePath("experiment.json");
        fileAction(window, "saveExperiment", project);
        QCOMPARE(load(project).at("ui").at("gpu_profile"), settings);
        view->findChild<QLineEdit *>("profileSamples")->setText("7");
        // Saving enters a dialog event loop, where a queued preview can start.
        // Experiment import requires that preview and report acceptance to finish.
        qInfo() << "Before experiment import" << window.busy() << window.statusBar()->currentMessage();
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        done.clear();
        fileAction(window, "openExperiment", project);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.takeLast()[0].toBool());
        QCOMPARE(view->settings(), settings);
        QCOMPARE(table->topLevelItemCount(), 0);
        view->findChild<QLineEdit *>("profileSamples")->setText("1000");
        view->findChild<QLineEdit *>("profileWarmup")->setText("0");
        done.clear();
        view->findChild<QAction *>("sampleGpuProfile")->trigger();
        QVERIFY(window.busy());
        QAction *cancel = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence("Escape"))
                cancel = action;
        QVERIFY(cancel);
        cancel->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(!done.takeLast()[0].toBool());
        QVERIFY(!window.busy());
        QCOMPARE(table->topLevelItemCount(), 0);
    }
    void frozenResultsAndFailedPass() {
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        testing::depthStencilCapture().save(path);
        auto frame = std::make_shared<Frame>(path.toStdWString());
        ReplayOptions options;
        options.until = 2000;
        options.warp = true;
        Replay replay(*frame, options);
        GpuProfileRequest request;
        request.samples = 1;
        request.warmup = 0;
        auto report = profileGpu(replay, *frame, request, dir.filePath("valid"));
        GpuProfileView view;
        view.setContext(frame, 1000, "original-key");
        QSignalSpy requests(&view, &GpuProfileView::readRequested);
        view.findChild<QAction *>("sampleGpuProfile")->trigger();
        QVERIFY(view.finish(requests.takeLast()[1].toULongLong(), report));
        view.findChild<QAction *>("sampleGpuProfile")->trigger();
        const auto pending = requests.takeLast()[1].toULongLong();
        view.setContext(frame, 1000, "edited-key");
        QVERIFY(!view.finish(pending, report));
        view.exportResult(dir.filePath("frozen.zip"));
        QFile zip(dir.filePath("frozen.zip"));
        QVERIFY(zip.open(QIODevice::ReadOnly));
        const auto bytes = zip.readAll();
        QVERIFY(bytes.contains("original-key"));
        QVERIFY(!bytes.contains("edited-key"));
        auto broken = testing::depthStencilCapture();
        broken.add(2100, 7, 0xffff, testing::statePack(Id(0), Id(1)));
        broken.save(dir.filePath("bad.gpa_frame"));
        Frame bad(dir.filePath("bad.gpa_frame").toStdWString());
        options.until = 2100;
        Replay failed(bad, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 profileGpu(failed, bad, request, dir.filePath("failed")));
        QVERIFY(!QFile::exists(dir.filePath("failed/profile.json")));
        // An exception closes the timestamp interval. A later profile on the
        // same device can still acquire valid samples after observer failure.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 replay.run({}, {}, [](Id id, bool after, auto *, const auto &) {
                                     if (id == 1000 && !after)
                                         throw std::runtime_error("Injected command failure");
                                 }));
        report = profileGpu(replay, *frame, request, dir.filePath("retry"));
        QCOMPARE(report["events"][0]["valid_samples"], Json(1));
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setApplicationName("FloraGPA-GpuProfile-Test");
    app.setOrganizationName("FloraGPA-Tests");
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        auto jobs = load(args[2]);
        Json results = Json::array();
        for (const auto &job : jobs) {
            try {
                results.push_back(run(job));
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        }
        save(args[3], results);
        return 0;
    }
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        for (auto widget : QApplication::topLevelWidgets())
            if (auto box = qobject_cast<QMessageBox *>(widget))
                box->accept();
    });
    dismiss.start(20);
    GpuProfileTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "GpuProfileTests.moc"
