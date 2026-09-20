#include "TextureEditCapture.h"
#include "application/Experiment.h"
#include "application/ExperimentReport.h"
#include "application/TextureInspector.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class ExperimentReportTests : public QObject {
    Q_OBJECT
  private slots:
    void boundariesAndRestart() {
        QTemporaryDir dir;
        auto c = textureEditCapture(28, 1, true);
        c.add(50, 7, 0x32, statePack(Id(0), Id(1), Id(24), uint8_t(1), 0.f, 0.f, 0.f, 0.f));
        c.add(80, 7, 0x34ef, statePack(Id(0), Id(1), Id(0)));
        auto path = dir.path() + "/report.gpa_frame";
        c.save(path);
        Frame frame(path.toStdWString());
        Experiment project(frame);
        project.setEnabled(frame, 200, false);
        project.setEnabled(frame, 100, true);
        project.setEnabled(frame, 50, false);
        project.setSetter(frame, 80, {{"input_layout", 0}});
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        project.apply(frame, options);
        Replay replay(frame, options);
        QCOMPARE(experimentReport(replay)["pending_events"], Json::array({50, 80, 100, 200}));
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            auto report = experimentReport(replay);
            QCOMPARE(report["applied_events"], Json::array({50, 80, 100}));
            QCOMPARE(report["pending_events"], Json::array({200}));
            QCOMPARE(report["cursor"], Json(4));
            QCOMPARE(report["revisions"], Json(4));
            TextureInspectionOptions view;
            view.preview = false;
            QCOMPARE(inspectTexture(replay, 20, view).metadata["experiment"], report);
        }
        options.before = true;
        Replay before(frame, options);
        before.run();
        before.inspectEventInputs(100, [] {});
        QCOMPARE(experimentReport(before)["applied_events"], Json::array({50, 80}));
        QCOMPARE(experimentReport(before)["pending_events"], Json::array({100, 200}));
        options.before = false;
        options.until = 200;
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(experimentReport(disabled)["applied_events"], Json::array({50, 80, 100, 200}));
        QVERIFY(experimentReport(disabled)["pending_events"].empty());
        options.suppressDraws = true;
        Replay suppressed(frame, options);
        suppressed.run();
        QCOMPARE(experimentReport(suppressed)["applied_events"], Json::array({50, 80, 100, 200}));
        QVERIFY(project.undo());
        project.apply(frame, options);
        Replay undone(frame, options);
        undone.run();
        QCOMPARE(experimentReport(undone)["applied_events"], Json::array({50, 100, 200}));
        QCOMPARE(experimentReport(undone)["cursor"], Json(3));
        QCOMPARE(experimentReport(undone)["revisions"], Json(4));
    }
    void observerFailures() {
        QTemporaryDir dir;
        auto c = textureEditCapture(28, 1, true);
        auto path = dir.path() + "/observer.gpa_frame";
        c.save(path);
        Frame frame(path.toStdWString());
        Experiment project(frame);
        project.setEnabled(frame, 100, true);
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        project.apply(frame, options);
        for (bool failAfter : {false, true}) {
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run({}, [&](Id, bool after, auto *, auto &) {
                if (after == failAfter)
                    throw std::runtime_error("Observer rejected boundary");
            }));
            QCOMPARE(experimentReport(replay)["applied_events"],
                     failAfter ? Json::array({100}) : Json::array());
        }
        ReplayOptions bare;
        bare.warp = true;
        Replay plain(frame, bare);
        QVERIFY(experimentReport(plain).is_null());
        Experiment empty(frame);
        empty.apply(frame, bare);
        Replay unedited(frame, bare);
        QCOMPARE(experimentReport(unedited)["cursor"], Json(0));
        QVERIFY(experimentReport(unedited)["pending_events"].empty());
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ExperimentReportTests test;
    return QTest::qExec(&test, argc, argv);
}
#include "ExperimentReportTests.moc"
