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
class WorkerRecoveryTests final : public QObject {
    Q_OBJECT
  private slots:
    void isolatedRecovery_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"missing", "stderr-tail", "stderr-lines", "crash", "invalid-report", "no-output", "missing-image", "cancel", "image-size", "image-pixels", "image-raw-missing", "image-raw-short", "image-raw-long", "image-raw-hash", "image-hash-missing", "image-unavailable-files", "image-unavailable-type", "image-valid", "image-async-cancel", "image-async-switch", "image-async-close", "image-async-destroy", "image-async-success"})
            QTest::newRow(mode) << QString(mode);
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
        child.start(executable, {"exerciseRecovery", "-o", log + ",txt"});
        QVERIFY(child.waitForStarted());
        const auto cleanup = qScopeGuard([&] { if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(); } });
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 240000);
        QFile file(log); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto contents = file.readAll();
        qInfo().noquote() << contents;
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }
    void exerciseRecovery() {
        const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
        if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
        QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
        const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
        QVERIFY(!QFile::exists(worker));
        const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
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
        if (mode.startsWith("image-")) {
            const auto fixture = QDir(root).filePath("fault-output");
            QVERIFY(QDir().mkpath(fixture));
            qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
            auto rgba = expected.convertToFormat(QImage::Format_RGBA8888);
            if (mode.startsWith("image-async-")) {
                rgba = QImage(4096, 4096, QImage::Format_RGBA8888);
                rgba.fill(QColor(53, 97, 179, 211));
            }
            QByteArray raw;
            for (int y = 0; y < rgba.height(); ++y)
                raw.append(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
            QJsonObject report{{"completed", true}, {"image_available", true},
                               {"width", rgba.width()}, {"height", rgba.height()},
                               {"rgba_sha256", QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex())}};
            if (mode == "image-size") report["width"] = rgba.width() + 1;
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
            const auto bytes = QJsonDocument(report).toJson();
            QCOMPARE(file.write(bytes), bytes.size());
        }
        QVERIFY(QFile::remove(worker));
        if (mode != "missing") QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker));
        done.clear();
        QElapsedTimer elapsed; elapsed.start();
        bool validationSeen = false, heartbeat = false;
        QString switchedPath, validationDirectory;
        QElapsedTimer validationElapsed;
        const auto observer = connect(window.statusBar(), &QStatusBar::messageChanged, &window,
            [&](const QString &message) {
                if (!mode.startsWith("image-async-") || message != "Validating image…" || validationSeen)
                    return;
                validationSeen = true;
                validationElapsed.start();
                QVERIFY(window.busy());
                // The event loop must run while image validation still owns the
                // request. Cancel also wins if its finished signal is queued.
                QTimer::singleShot(0, &window, [&] {
                    heartbeat = true;
                    QVERIFY(window.busy());
                    if (mode == "image-async-destroy") {
                        QFile pathFile(QDir(root).filePath("fault-output/validation-directory.txt"));
                        QVERIFY(pathFile.open(QIODevice::ReadOnly));
                        validationDirectory = QString::fromLocal8Bit(pathFile.readAll());
                        QVERIFY(QFileInfo::exists(validationDirectory));
                        owner.reset();
                    } else if (mode == "image-async-success") {
                        // Leave the validator running; it must accept the full
                        // large image, not just yield and bypass verification.
                    } else if (mode == "image-async-close") {
                        window.close();
                    } else if (mode == "image-async-switch") {
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
        if (mode == "image-async-destroy") {
            QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
            QVERIFY(validationSeen && heartbeat);
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000);
            QCOMPARE(done.size(), 0);
            qInfo() << "Destroyed validating window; directory released after" << validationElapsed.elapsed() << "ms";
            return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), mode == "image-async-switch" ? 2 : 1, mode == "timeout" ? 210000 : 30000);
        disconnect(observer);
        if (mode.startsWith("image-async-")) {
            QVERIFY(validationSeen && heartbeat);
            QCOMPARE(done.first()[0].toBool(), mode == "image-async-success");
            qInfo() << "Image event loop heartbeat; completion after" << validationElapsed.elapsed() << "ms";
        }
        if (mode == "image-async-switch") {
            QVERIFY(done.last()[0].toBool());
            QCOMPARE(window.capturePath(), switchedPath);
            QCOMPARE(image->image(), expected);
            QTest::qWait(100);
            QCOMPARE(done.size(), 2);
            return;
        }
        QCOMPARE(done.last()[0].toBool(), mode == "image-valid" || mode == "image-async-success");
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
        } else
            QCOMPARE(image->image(), expected); // A failure never accepts a replacement image.
        if (mode != "image-valid" && mode != "image-async-success") {
            QCOMPARE(label->text(), expectedLabel);
            QCOMPARE(label->toolTip(), expectedTooltip);
        }
        if (mode.startsWith("image-") && mode != "image-valid" && !mode.startsWith("image-async-"))
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
