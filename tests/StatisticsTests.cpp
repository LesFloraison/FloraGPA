#include "DepthStencilCapture.h"
#include "app/AnnotationsView.h"
#include "app/Appearance.h"
#include "app/MainWindow.h"
#include "app/StatisticsView.h"
#include "application/GpuStatistics.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
ReplayOptions options(Id start, Id end, bool single = false) {
    ReplayOptions o;
    o.warp = true;
    o.until = end;
    o.measurement = ReplayOptions::Measurement{start, end, single};
    return o;
}
QString metric(StatisticsView *view, const QString &name) {
    auto tree = view->findChild<QTreeWidget *>("statisticsValues");
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        if (tree->topLevelItem(i)->text(0) == name)
            return tree->topLevelItem(i)->text(1);
    return {};
}
} // namespace
class StatisticsTests final : public QObject {
    Q_OBJECT
  private slots:
    void boundariesAndDisabledDraw() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/frame.gpa_frame";
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions plain;
            plain.warp = warp;
            Replay baseline(frame, plain);
            baseline.run();
            const auto pixels = baseline.output().rgba;
            for (auto range :
                 {ReplayOptions::Measurement{1000, 1000, true}, ReplayOptions::Measurement{800, 2000, false},
                  ReplayOptions::Measurement{800, 810, false}}) {
                auto o = options(range.start, range.end, range.singleEvent);
                o.warp = warp;
                Replay replay(frame, o);
                replay.run();
                auto report = gpuStatisticsReport(replay);
                const auto vertices = range.singleEvent ? 3u : range.end == 2000 ? 6u : 0u;
                QCOMPARE(report["pipeline"]["ia_vertices"].get<unsigned>(), vertices);
                QCOMPARE(report["pipeline"]["ia_primitives"].get<unsigned>(), vertices / 3);
                QCOMPARE(report["replay_generation"].get<unsigned>(), 1u);
                QVERIFY(report["timing"]["available"].get<bool>());
                QVERIFY(report["timing"]["elapsed_ms"].get<double>() >= 0.0);
                if (range.end == 2000)
                    QCOMPARE(replay.output().rgba, pixels);
                replay.run();
                report = gpuStatisticsReport(replay);
                QCOMPARE(report["replay_generation"].get<unsigned>(), 2u);
                QCOMPARE(report["pipeline"]["ia_vertices"].get<unsigned>(), vertices);
            }
            auto o = options(800, 2000);
            o.warp = warp;
            o.disabled.insert(2000);
            Replay disabled(frame, o);
            disabled.run();
            const auto report = gpuStatisticsReport(disabled);
            QCOMPARE(report["pipeline"]["ia_vertices"].get<unsigned>(), 3u);
            QVERIFY(report["range"]["disabled_commands"] == Json::array({2000}));
            QCOMPARE(report["replay_counts"]["Draw_records"].get<unsigned>(), 2u);
            QCOMPARE(report["replay_counts"]["experiment_disabled_Draw"].get<unsigned>(), 1u);
        }
    }
    void failedSampleAndRetry() {
        QTemporaryDir dir;
        auto path = dir.path() + "/frame.gpa_frame";
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        for (bool single : {false, true}) {
            Replay replay(frame, options(single ? 1000 : 800, single ? 1000 : 2000, single));
            replay.run();
            QVERIFY(replay.measurementResult());
            // Range queries are still active here; single-event queries have ended.
            // Neither path may publish the preceding successful sample after failure.
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     replay.run({}, [](Id id, bool after, auto *, const auto &) {
                                         if (id == 1000 && after)
                                             throw std::runtime_error("Injected observer failure");
                                     }));
            QVERIFY(!replay.measurementResult());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, gpuStatisticsReport(replay));
            replay.run();
            QCOMPARE(gpuStatisticsReport(replay)["pipeline"]["ia_vertices"].get<unsigned>(),
                     single ? 3u : 6u);
            QCOMPARE(replay.generation(), 3ull);
        }
        for (auto range :
             {ReplayOptions::Measurement{1, 2000, false}, ReplayOptions::Measurement{2000, 800, false},
              ReplayOptions::Measurement{800, 800, true}}) {
            Replay replay(frame, options(range.start, range.end, range.singleEvent));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
            QVERIFY(!replay.measurementResult());
        }
    }
    void staleResultsAndExport() {
        QTemporaryDir dir;
        auto path = dir.path() + "/frame.gpa_frame";
        depthStencilCapture().save(path);
        auto frame = std::make_shared<Frame>(path.toStdWString());
        Replay replay(*frame, options(1000, 1000, true));
        replay.run();
        auto report = gpuStatisticsReport(replay);
        // Above the JSON-double exact-integer range, presentation/export must stay exact.
        report["pipeline"]["ia_vertices"] = uint64_t(9007199254740993ull);
        StatisticsView view;
        view.setSelection(frame, 1000);
        QSignalSpy requests(&view, &StatisticsView::readRequested);
        auto event = view.findChild<QAction *>("measureStatisticsEvent");
        auto exportAction = view.findChild<QAction *>("exportStatistics");
        event->trigger();
        QCOMPARE(requests.size(), 1);
        auto token = requests.takeFirst()[3].toULongLong();
        view.invalidate();
        QVERIFY(!view.finish(token, report));
        QVERIFY(!exportAction->isEnabled());
        event->trigger();
        token = requests.takeFirst()[3].toULongLong();
        QVERIFY(view.finish(token, report));
        QCOMPARE(metric(&view, "ia_vertices"), QString("9007199254740993"));
        view.setWorkerBusy(true);
        QVERIFY(!event->isEnabled());
        QVERIFY(!exportAction->isEnabled());
        view.setWorkerBusy(false);
        const auto destination = dir.path() + "/export";
        QVERIFY(QDir().mkpath(destination));
        bool accepted = false;
        QTimer::singleShot(100, [&] {
            for (auto widget : QApplication::topLevelWidgets())
                if (auto dialog = qobject_cast<QFileDialog *>(widget)) {
                    dialog->setDirectory(dir.path());
                    dialog->findChild<QLineEdit *>("fileNameEdit")->setText(destination);
                    accepted = QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                }
        });
        exportAction->trigger();
        QVERIFY(accepted);
        QFile json(destination + "/statistics.json");
        QVERIFY2(json.open(QIODevice::ReadOnly),
                 qPrintable(view.findChild<QLabel *>("statisticsSummary")->text() + " " +
                            view.findChild<QLabel *>("statisticsSummary")->toolTip()));
        const auto bytes = json.readAll();
        QVERIFY(Json::parse(bytes.constData(), bytes.constData() + bytes.size()) == report);
        QFile csv(destination + "/statistics.csv");
        QVERIFY(csv.open(QIODevice::ReadOnly));
        QVERIFY(csv.readAll().contains("ia_vertices,9007199254740993,count"));
        view.setSelection(frame, 800);
        QVERIFY(!event->isEnabled());
        QVERIFY(!exportAction->isEnabled());
        view.setRange(2000, 800);
        view.findChild<QAction *>("measureStatisticsRange")->trigger();
        QVERIFY(requests.empty());
        view.setSelection(nullptr, 0);
        QVERIFY(!view.findChild<QAction *>("measureStatisticsFrame")->isEnabled());
    }
    void annotationHandoffAndWorker() {
        QTemporaryDir dir;
        auto c = depthStencilCapture();
        auto begin = statePack(Id(0), Id(600), int32_t(0), uint32_t(10));
        for (uint16_t ch : {uint16_t('P'), uint16_t('a'), uint16_t('s'), uint16_t('s'), uint16_t(0)})
            append(begin, ch);
        c.add(790, 7, 0x327b, begin);
        c.add(2100, 7, 0x327c, statePack(Id(0), Id(600), int32_t(0)));
        auto path = dir.path() + "/frame.gpa_frame";
        c.save(path);
        MainWindow window;
        window.resize(1440, 900);
        window.show();
        QSignalSpy loaded(&window, &MainWindow::captureLoaded), tasks(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        window.findChild<QAction *>("showAnnotations")->trigger();
        auto annotations = window.findChild<AnnotationsView *>();
        QSignalSpy read(annotations, &AnnotationsView::inspectionFinished);
        annotations->findChild<QAction *>("readAnnotations")->trigger();
        QTRY_COMPARE(read.size(), 1);
        QVERIFY(read.takeFirst()[0].toBool());
        auto tree = annotations->findChild<QTreeWidget *>("annotationNodes");
        QTreeWidgetItemIterator it(tree);
        while (*it && (*it)->data(0, Qt::UserRole).toULongLong() != 790)
            ++it;
        QVERIFY(*it);
        tree->setCurrentItem(*it);
        auto range = annotations->findChild<QAction *>("annotationRangeMetrics");
        QVERIFY(range->isEnabled());
        range->trigger();
        auto view = window.findChild<StatisticsView *>();
        QVERIFY(view->isVisible());
        QCOMPARE(view->findChild<QLineEdit *>("statisticsStart")->text(), QString("790"));
        QCOMPARE(view->findChild<QLineEdit *>("statisticsEnd")->text(), QString("2100"));
        QVERIFY(tasks.empty()); // Handoff prefills; it does not initiate GPU work.
        QSignalSpy done(view, &StatisticsView::inspectionFinished);
        view->findChild<QAction *>("measureStatisticsRange")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(done.takeFirst()[0].toBool());
        QCOMPARE(metric(view, "ia_vertices"), QString("6"));
        view->findChild<QAction *>("measureStatisticsFrame")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(done.takeFirst()[0].toBool());
        QCOMPARE(metric(view, "ia_vertices"), QString("6"));
        const auto evidence = qEnvironmentVariable("FLORA_STATISTICS_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            QVERIFY(window.grab().save(evidence + "/statistics-main-window.png"));
        }
        auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        tasks.clear();
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index);
                break;
            }
        }
        QCOMPARE(api->currentIndex().data(Qt::UserRole).toULongLong(), 1000ull);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        tasks.clear();
        window.findChild<QComboBox *>("replayAdapter")->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!tasks.empty(), 30000);
        QVERIFY(tasks.takeLast()[0].toBool());
        window.findChild<QAction *>("showGpuStatistics")->trigger();
        view->findChild<QAction *>("measureStatisticsEvent")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(done.takeFirst()[0].toBool());
        QCOMPARE(metric(view, "ia_vertices"), QString("3"));
        QVERIFY(view->findChild<QLabel *>("statisticsSummary")->text().contains("Event 1000 · warp"));
        view->findChild<QAction *>("measureStatisticsEvent")->trigger();
        QAction *cancel = nullptr;
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Cancel")
                cancel = action;
        QVERIFY(cancel && cancel->isEnabled());
        cancel->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(!done.takeFirst()[0].toBool());
        QVERIFY(!view->findChild<QAction *>("exportStatistics")->isEnabled());
        view->findChild<QAction *>("measureStatisticsEvent")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(done.takeFirst()[0].toBool());
        QCOMPARE(metric(view, "ia_vertices"), QString("3"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("FloraGPA-StatisticsTests");
    app.setApplicationName("FloraGPA-StatisticsTests");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    applyAppearance(app);
    StatisticsTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "StatisticsTests.moc"
