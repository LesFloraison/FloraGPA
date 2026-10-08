#include "MsaaCapture.h"
#include "app/MainWindow.h"
#include <QDirIterator>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QAction>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QtTest>
using namespace flora;
#include "BufferRecovery.h"
#include "PayloadRecovery.h"
#include "DiagnosticRecovery.h"
#include "ThumbnailRecovery.h"
#include "CatalogRecovery.h"
#include "ExportRecovery.h"
class WorkerRecoveryTests final : public QObject {
    Q_OBJECT
  private slots:
    void isolatedRecovery_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"export-valid", "export-short", "export-long", "export-hash", "export-metadata", "export-missing-metadata",
                               "export-async-cancel", "export-async-switch", "export-async-close", "export-async-destroy", "export-async-success"})
            QTest::newRow(mode) << QString(mode);
        for (const auto mode : {"image-resource-missing", "image-resource-unknown", "image-resource-kind"})
            QTest::newRow(mode) << QString(mode);
        for (const auto mode : {"missing", "stderr-tail", "stderr-lines", "crash", "invalid-report", "no-output", "missing-image", "cancel", "image-size", "image-pixels", "image-raw-missing", "image-raw-short", "image-raw-long", "image-raw-hash", "image-hash-missing", "image-unavailable-files", "image-unavailable-type", "image-valid", "image-async-cancel", "image-async-switch", "image-async-close", "image-async-destroy", "image-async-success"})
            QTest::newRow(mode) << QString(mode);
        for (auto mode : {"buffer-valid", "buffer-short", "buffer-hash", "buffer-resource", "buffer-range",
                          "buffer-boundary", "buffer-async-cancel", "buffer-async-switch", "buffer-async-close",
                          "buffer-async-destroy", "buffer-async-success"})
            QTest::newRow(mode) << QString(mode);
        for (auto mode : {"report-valid", "report-syntax", "report-root", "report-completed",
                          "report-async-cancel", "report-async-switch", "report-async-close",
                          "report-async-destroy", "report-async-success"})
            QTest::newRow(mode) << QString(mode);
        for (auto mode : {"payload-valid", "payload-missing", "payload-syntax", "payload-root",
                          "payload-async-cancel", "payload-async-switch", "payload-async-close",
                          "payload-async-destroy", "payload-async-success"})
            QTest::newRow(mode) << QString(mode);
        for (const auto kind : {QString("coverage"), QString("quad")}) {
            for (const auto suffix : {"valid", "missing", "corrupt", "dimensions", "event",
                                      "async-cancel", "async-switch", "async-close", "async-destroy", "async-success"}) {
                const auto mode = kind + '-' + suffix;
                QTest::newRow(qPrintable(mode)) << mode;
            }
        }
        for (const auto mode : {"quad-short", "quad-long", "quad-overflow"})
            QTest::newRow(mode) << QString(mode);
        for (const auto suffix : {"valid", "missing", "identity", "subresource", "omitted", "duplicate", "oversized",
                                  "async-success", "async-cancel", "async-switch", "async-close", "async-destroy"}) {
            const auto mode = QString("thumbnail-") + suffix;
            QTest::newRow(qPrintable(mode)) << mode;
        }
        for (const auto suffix : {"valid", "missing", "syntax", "root", "late-invalid", "async-success", "async-cancel", "async-switch", "async-close", "async-destroy"}) {
            const auto mode = QString("catalog-") + suffix;
            QTest::newRow(qPrintable(mode)) << mode;
        }
        if (qEnvironmentVariableIsSet("FLORA_TEST_WORKER_TIMEOUT")) QTest::newRow("timeout") << QString("timeout");
    }
    void isolatedRecovery() {
        QFETCH(QString, mode);
        QTemporaryDir isolation;
        QVERIFY(isolation.isValid());
        const QDir source(QCoreApplication::applicationDirPath());
        const auto executable = isolation.filePath("FloraWorkerRecoveryTests.exe");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
        // A packaged test carries its runtime with it; build-tree tests use the
        // CTest Qt PATH. Never rename or replace the installed/built worker.
        QDirIterator files(source.path(), {"*.dll"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto file = files.next();
            const auto target = isolation.filePath(source.relativeFilePath(file));
            QVERIFY(QDir().mkpath(QFileInfo(target).path()));
            QVERIFY(QFile::copy(file, target));
        }
        QProcess child;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("FLORA_FAULT_MODE", mode);
        env.insert("FLORA_FAULT_ROOT", isolation.path());
        env.insert("FLORA_REAL_WORKER", source.filePath("FloraGPA.Worker.exe"));
        env.insert("FLORA_FAULT_WORKER", source.filePath("FloraFaultWorker.exe"));
        child.setProcessEnvironment(env);
        child.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
        const auto log = isolation.filePath("child.txt");
        const auto exercise = mode.startsWith("export-") ? "exerciseExportRecovery" :
                              mode.startsWith("catalog-") ? "exerciseCatalogRecovery" :
                              mode.startsWith("thumbnail-") ? "exerciseThumbnailRecovery" :
                              mode.startsWith("coverage-") || mode.startsWith("quad-") ? "exerciseDiagnosticRecovery" :
                              mode.startsWith("payload-") ? "exercisePayloadRecovery" :
                              mode.startsWith("buffer-") ? "exerciseBufferRecovery" : "exerciseRecovery";
        child.start(executable, {exercise, "-o", log + ",txt"});
        QVERIFY(child.waitForStarted());
        const auto cleanup = qScopeGuard([&] { if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(); } });
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 240000);
        QFile file(log); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto contents = file.readAll();
        qInfo().noquote() << contents;
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }
    void exerciseBufferRecovery() { ::exerciseBufferRecovery(); }
    void exercisePayloadRecovery() { ::exercisePayloadRecovery(); }
    void exerciseDiagnosticRecovery() { ::exerciseDiagnosticRecovery(); }
    void exerciseThumbnailRecovery() { ::exerciseThumbnailRecovery(); }
    void exerciseCatalogRecovery() { ::exerciseCatalogRecovery(); }
    void exerciseExportRecovery() { ::exerciseExportRecovery(); }
    void exerciseRecovery() {
        const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
        if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
        QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
        const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
        QVERIFY(!QFile::exists(worker));
        const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
        const bool reportMode = mode.startsWith("report-");
        const bool reportAsync = mode.startsWith("report-async-");
        const bool asynchronous = reportAsync || mode.startsWith("image-async-");
        const bool accepted = mode == "image-valid" || mode == "report-valid" ||
                              (asynchronous && mode.endsWith("-success"));
        const auto path = QDir(root).filePath("frame.gpa_frame");
        testing::msaaOutputCapture(false).save(path);
        auto owner = std::make_unique<MainWindow>();
        auto &window = *owner;
        QSignalSpy done(&window, &MainWindow::taskFinished);
        QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
        window.openCapture(path);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(done.last()[0].toBool());
        const auto image = window.findChild<ImageView *>("frameOutput");
        QVERIFY(image && !image->image().isNull());
        const auto expected = image->image();
        const auto label = window.findChild<QLabel *>("frameOutputLabel");
        QVERIFY(label);
        const auto expectedLabel = label->text(), expectedTooltip = label->toolTip();
        if (mode.startsWith("image-") || reportMode) {
            const auto fixture = QDir(root).filePath("fault-output");
            QVERIFY(QDir().mkpath(fixture));
            qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
            auto rgba = expected.convertToFormat(QImage::Format_RGBA8888);
            if (mode.startsWith("image-async-")) {
                rgba = QImage(4096, 4096, QImage::Format_RGBA8888);
                rgba.fill(QColor(53, 97, 179, 211));
            }
            if (reportAsync) rgba.fill(QColor(53, 97, 179, 211));
            QByteArray raw;
            for (int y = 0; y < rgba.height(); ++y)
                raw.append(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
            QJsonObject report{{"completed", true}, {"image_available", true},
                               {"resource", "20"},
                               {"width", rgba.width()}, {"height", rgba.height()},
                               {"rgba_sha256", QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex())}};
            if (mode == "image-size") report["width"] = rgba.width() + 1;
            if (mode == "image-resource-missing") report.remove("resource");
            if (mode == "image-resource-unknown") report["resource"] = "18446744073709551615";
            if (mode == "image-resource-kind") report["resource"] = "1";
            if (mode == "image-hash-missing") report.remove("rgba_sha256");
            if (mode == "image-unavailable-files") report["image_available"] = false;
            if (mode == "image-unavailable-type") report["image_available"] = "false";
            if (mode == "image-pixels") rgba.bits()[0] ^= 0xff;
            QVERIFY(rgba.save(fixture + "/frame.png"));
            if (mode == "image-raw-short") raw.chop(1);
            if (mode == "image-raw-long") raw.append('x');
            if (mode == "image-raw-hash") raw[0] = char(raw[0] ^ 0xff);
            if (mode != "image-raw-missing") {
                QFile file(fixture + "/frame.rgba"); QVERIFY(file.open(QIODevice::WriteOnly));
                QCOMPARE(file.write(raw), raw.size());
            }
            QFile file(fixture + "/report.json"); QVERIFY(file.open(QIODevice::WriteOnly));
            if (reportAsync) report["padding"] = QString(64 * 1024 * 1024, QChar('x'));
            if (mode == "report-completed") report["completed"] = "true";
            auto bytes = QJsonDocument(report).toJson();
            if (mode == "report-root") bytes = "[]";
            if (mode == "report-syntax") bytes.chop(2);
            QCOMPARE(file.write(bytes), bytes.size());
        }
        QVERIFY(QFile::remove(worker));
        if (mode != "missing") QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker));
        done.clear();
        QElapsedTimer elapsed; elapsed.start();
        bool validationSeen = false, heartbeat = false;
        QString switchedPath, validationDirectory;
        QElapsedTimer validationElapsed;
        QTimer pulse;
        int reportTicks = 0;
        qint64 lastTick = 0, maximumGap = 0;
        connect(&pulse, &QTimer::timeout, &window, [&] {
            if (!window.busy()) return;
            const auto now = validationElapsed.elapsed();
            maximumGap = std::max(maximumGap, now - lastTick);
            lastTick = now;
            ++reportTicks;
        });
        const auto observer = connect(window.statusBar(), &QStatusBar::messageChanged, &window,
            [&](const QString &message) {
                if (!asynchronous || message != (reportAsync ? "Reading report…" : "Validating image…") || validationSeen)
                    return;
                validationSeen = true;
                validationElapsed.start();
                if (reportAsync) pulse.start(5);
                QVERIFY(window.busy());
                // The event loop must run while image validation still owns the
                // request. Cancel also wins if its finished signal is queued.
                QTimer::singleShot(0, &window, [&] {
                    heartbeat = true;
                    QVERIFY(window.busy());
                    if ((asynchronous && mode.endsWith("-destroy"))) {
                        QFile pathFile(QDir(root).filePath("fault-output/validation-directory.txt"));
                        QVERIFY(pathFile.open(QIODevice::ReadOnly));
                        validationDirectory = QString::fromLocal8Bit(pathFile.readAll());
                        QVERIFY(QFileInfo::exists(validationDirectory));
                        owner.reset();
                    } else if (accepted) {
                        // Leave the validator running; it must accept the full
                        // large image, not just yield and bypass verification.
                    } else if ((asynchronous && mode.endsWith("-close"))) {
                        window.close();
                    } else if ((asynchronous && mode.endsWith("-switch"))) {
                        QVERIFY(QFile::remove(worker));
                        QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
                        switchedPath = QDir(root).filePath("second.gpa_frame");
                        testing::msaaOutputCapture(false).save(switchedPath);
                        window.openCapture(switchedPath);
                    } else {
                        for (auto action : window.findChildren<QAction *>())
                            if (action->shortcut() == QKeySequence(Qt::Key_Escape)) {
                                QVERIFY(action->isEnabled());
                                action->trigger();
                            }
                    }
                });
            });
        window.replay();
        if (mode == "cancel") {
            QTest::qWait(100);
            QVERIFY(window.busy());
            QAction *cancel = nullptr;
            for (auto action : window.findChildren<QAction *>())
                if (action->shortcut() == QKeySequence(Qt::Key_Escape)) cancel = action;
            QVERIFY(cancel && cancel->isEnabled());
            cancel->trigger();
        }
        if ((asynchronous && mode.endsWith("-destroy"))) {
            QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
            QVERIFY(validationSeen && heartbeat);
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000);
            QCOMPARE(done.size(), 0);
            qInfo() << "Destroyed validating window; directory released after" << validationElapsed.elapsed() << "ms";
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), (asynchronous && mode.endsWith("-switch")) ? 2 : 1, mode == "timeout" ? 210000 : 30000);
        disconnect(observer);
        pulse.stop();
        if (mode == "report-async-success") {
            QVERIFY(reportTicks >= 3);
            qInfo() << "Report busy heartbeat ticks" << reportTicks << "maximum_gap_ms" << maximumGap;
        }
        if (asynchronous) {
            QVERIFY(validationSeen && heartbeat);
            QCOMPARE(done.first()[0].toBool(), accepted);
            qInfo() << "Artifact event loop heartbeat; completion after" << validationElapsed.elapsed() << "ms";
        }
        if ((asynchronous && mode.endsWith("-switch"))) {
            QVERIFY(done.last()[0].toBool());
            QCOMPARE(window.capturePath(), switchedPath);
            QCOMPARE(image->image(), expected);
            QTest::qWait(100);
            QCOMPARE(done.size(), 2);
            return;
        }
        QCOMPARE(done.last()[0].toBool(), accepted);
        QVERIFY(!window.busy());
        unsigned checkedActions = 0;
        for (auto action : window.findChildren<QAction *>()) {
            if (action->shortcut() == QKeySequence(Qt::Key_F5)) { QVERIFY(action->isEnabled()); ++checkedActions; }
            if (action->shortcut() == QKeySequence(Qt::Key_Escape)) { QVERIFY(!action->isEnabled()); ++checkedActions; }
        }
        QCOMPARE(checkedActions, 2u);
        QCOMPARE(window.capturePath(), path);
        const auto error = window.statusBar()->currentMessage();
        QVERIFY(!error.isEmpty());
        if (mode == "stderr-tail") QCOMPARE(error, QString("Fault sentinel without newline"));
        if (mode == "stderr-lines") QCOMPARE(error, QString("Fault sentinel with newline"));
        if (mode == "timeout") {
            QVERIFY(elapsed.elapsed() >= 170000);
            QCOMPARE(error, QString("Worker timed out."));
        }
        if (mode == "cancel") QCOMPARE(error, QString("Cancelled"));
        if (mode == "image-async-success") {
            QImage large(4096, 4096, QImage::Format_RGBA8888);
            large.fill(QColor(53, 97, 179, 211));
            QCOMPARE(image->image(), large);
        } else if (mode == "report-async-success") {
            auto changed = expected.convertToFormat(QImage::Format_RGBA8888);
            changed.fill(QColor(53, 97, 179, 211));
            QCOMPARE(image->image(), changed);
        } else
            QCOMPARE(image->image(), expected); // A failure never accepts a replacement image.
        if (!accepted) {
            QCOMPARE(label->text(), expectedLabel);
            QCOMPARE(label->toolTip(), expectedTooltip);
        }
        if ((mode.startsWith("image-") || reportMode) && !accepted && !asynchronous)
            QVERIFY2(error.startsWith("Worker "), qPrintable(error));
        qInfo().noquote() << "fault_observation" << mode << "elapsed_ms" << elapsed.elapsed() << error;
        if (QFile::exists(worker)) QVERIFY(QFile::remove(worker));
        QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
        done.clear();
        window.replay();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
        QVERIFY(done.last()[0].toBool());
        QVERIFY(!window.busy());
        QCOMPARE(image->image(), expected);
        QTest::qWait(100);
        QCOMPARE(done.size(), 1); // No stale completion from the failed worker.
    }
};
QTEST_MAIN(WorkerRecoveryTests)
#include "WorkerRecoveryTests.moc"
