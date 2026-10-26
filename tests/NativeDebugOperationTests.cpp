#include "NativeDebugFixture.h"
#include "app/NativeDebugOperationController.h"
#include "application/ZipArchive.h"
#include <QScopeGuard>
#include <QSemaphore>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
class NativeDebugOperationTests final : public QObject {
    Q_OBJECT
  private slots:
    void interruptedHitCountCache() {
        test::NativeDebugFixture fixture;
        NativeDebugSession session(fixture.job, fixture.result, fixture.artifacts);
        auto &model = session.model();
        const auto initial = model.registerValues();
        const SourceTrace::Point point{0, 6};
        int checks = 0;
        model.source().setCheckpoint([&] {
            // Interrupt after the first scan, while the encounter cache is built.
            if (++checks > int(model.size()) + 8)
                throw OperationCancelled();
        });
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, model.source().encounterCount(model.size() - 1, point));
        model.source().setCheckpoint({});
        QCOMPARE(model.source().encounterCount(model.size() - 1, point), size_t(3));
        QCOMPARE(model.registerValues(), initial);
    }
    void cancelledProjectionAndRetry() {
        test::NativeDebugFixture fixture;
        NativeDebugSession session(fixture.job, fixture.result, fixture.artifacts);
        auto &model = session.model();
        const auto initial = model.registerValues(), config = model.configuration();
        int checks = 0;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 model.inspect(NativeDebugCommand::Position, model.size(), config, "uint",
                                               false, [&] { return ++checks == 12; }));
        QCOMPARE(model.position(), size_t(0));
        QCOMPARE(model.registerValues(), initial);
        const auto final = model.inspect(NativeDebugCommand::Position, model.size(), config, "uint");
        QCOMPARE(final.at("state").at("state_boundary"), Json("terminal_group"));
        const auto restored = model.inspect(NativeDebugCommand::Position, 0, config, "uint");
        QCOMPARE(restored.at("registers"), initial);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 model.inspect(static_cast<NativeDebugCommand>(100), 0, config, "uint"));
    }
    void archiveCancellationPreservesDestination() {
        QTemporaryDir directory;
        const auto path = directory.filePath("session.zip");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("original");
        }
        const auto source = directory.filePath("large.bin");
        {
            QFile file(source);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(8 * 1024 * 1024, 'a'));
        }
        for (bool fileBacked : {false, true}) {
            int checks = 0;
            const ZipEntry entry = fileBacked ? ZipEntry{"test.bin", source, {}}
                                              : ZipEntry{"test.bin", {}, QByteArray(8 * 1024 * 1024, 'a')};
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     writeZipArchive(path, {entry}, [&] { return ++checks == 5; }));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("original"));
            QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), qsizetype(2));
        }
        writeZipArchive(path, {{"test.bin", {}, "done"}});
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().startsWith("PK"));
    }
    void asynchronousCancellationDeadlineAndExport() {
        test::NativeDebugFixture fixture;
        auto session = std::make_shared<NativeDebugSession>(fixture.job, fixture.result, fixture.artifacts);
        const auto config = session->model().configuration();
        const auto count = session->model().size();
        NativeDebugOperationController controller;
        NativeDebugOperationOutcome outcome;
        int completed = 0, heartbeat = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++heartbeat; });
        timer.start(0);
        connect(&controller, &NativeDebugOperationController::finished, this, [&](const auto &value) {
            outcome = value;
            ++completed;
        });
        auto pool = QThreadPool::globalInstance();
        const auto threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore entered, release;
        auto blocker = QtConcurrent::run([&] {
            entered.release();
            release.acquire();
        });
        entered.acquire();
        auto restorePool = qScopeGuard([&] {
            release.release();
            blocker.waitForFinished();
            pool->setMaxThreadCount(threads);
        });
        controller.start(session, NativeDebugCommand::Position, count, config, "uint");
        QVERIFY(controller.active());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 controller.start(session, NativeDebugCommand::Position, 0, config, "uint"));
        controller.cancel();
        release.release();
        blocker.waitForFinished();
        pool->setMaxThreadCount(threads);
        restorePool.dismiss();
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 10000);
        QVERIFY(outcome.cancelled && outcome.snapshot.is_null());
        QCOMPARE(session->model().position(), size_t(0));
        controller.start(session, NativeDebugCommand::Position, count, config, "uint", {}, 0.0000001);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 10000);
        QVERIFY(outcome.error.contains("timed out"));
        QCOMPARE(session->model().position(), size_t(0));
        controller.start(session, NativeDebugCommand::Position, count, config, "uint");
        QTRY_COMPARE_WITH_TIMEOUT(completed, 3, 10000);
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.snapshot.at("position"), Json(count));
        QVERIFY(heartbeat > 0);
        QTemporaryDir directory;
        const auto archive = directory.filePath("session.zip");
        controller.start(session, NativeDebugCommand::Position, count, config, "uint", archive);
        QTRY_COMPARE_WITH_TIMEOUT(completed, 4, 10000);
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QCOMPARE(outcome.exported, archive);
        QVERIFY(QFileInfo::exists(archive));
        QVERIFY(!outcome.snapshot.contains("report"));
    }
    void destructorReleasesSession() {
        test::NativeDebugFixture fixture;
        auto session = std::make_shared<NativeDebugSession>(fixture.job, fixture.result, fixture.artifacts);
        const auto path = session->tracePath();
        {
            NativeDebugOperationController controller;
            controller.start(session, NativeDebugCommand::Position, session->model().size(),
                             session->model().configuration(), "uint");
            session.reset();
        }
        QVERIFY(!QFileInfo::exists(path));
    }
    void publishedArchiveWinsLateCancellation() {
        test::NativeDebugFixture fixture;
        auto session = std::make_shared<NativeDebugSession>(fixture.job, fixture.result, fixture.artifacts);
        QTemporaryDir directory;
        const auto archive = directory.filePath("published.zip");
        NativeDebugOperationController controller;
        NativeDebugOperationOutcome outcome;
        bool delivered = false;
        connect(&controller, &NativeDebugOperationController::finished, this, [&](const auto &value) {
            outcome = value;
            delivered = true;
        });
        controller.start(session, NativeDebugCommand::Position, 0, session->model().configuration(), "uint",
                         archive);
        QElapsedTimer deadline;
        deadline.start();
        // Deliberately withhold event delivery until the archive is committed.
        while (!QFileInfo::exists(archive) && deadline.elapsed() < 10000)
            QThread::msleep(1);
        QVERIFY(QFileInfo::exists(archive));
        QVERIFY(controller.active());
        controller.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(delivered, 10000);
        QVERIFY2(outcome.error.isEmpty(), qPrintable(outcome.error));
        QVERIFY(!outcome.cancelled);
        QCOMPARE(outcome.exported, archive);
    }
};
QTEST_GUILESS_MAIN(NativeDebugOperationTests)
#include "NativeDebugOperationTests.moc"
