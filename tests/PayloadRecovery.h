#pragma once
#include "StreamCapture.h"
// Runs in a parent-owned isolated executable directory with real/fault Workers.
inline void exercisePayloadRecovery() {
    const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
    const bool asynchronous = mode.startsWith("payload-async-");
    const bool accepted = mode == "payload-valid" || mode == "payload-async-success";
    const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
    QVERIFY(!QFile::exists(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    const auto capture = QDir(root).filePath("stream.gpa_frame");
    testing::streamCapture().save(capture);
    auto owner = std::make_unique<MainWindow>();
    auto &window = *owner;
    QSignalSpy done(&window, &MainWindow::taskFinished);
    window.openCapture(capture);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    auto api = window.findChild<QTableView *>("apiLog");
    QVERIFY(api);
    api->setCurrentIndex(api->model()->index(2, 0));
    auto inspect = window.findChild<QAction *>("inspectGeometry");
    QVERIFY(inspect && inspect->isEnabled());
    done.clear(); inspect->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
    auto model = window.findChild<QTableView *>("geometryTable")->model();
    QCOMPARE(model->rowCount(), 6);
    const auto expectedFirst = model->index(0, 0).data();
    const auto expectedImage = window.findChild<ImageView *>("frameOutput")->image();
    const auto fixture = QDir(root).filePath("fault-output");
    QVERIFY(QDir().mkpath(fixture));
    qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
    auto save = [&](const QString &name, const QByteArray &bytes) {
        QFile file(fixture + '/' + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    };
    save("report.json", "{\"completed\":true}");
    QByteArray payload("{\"event\":\"200\",\"vertex_references\":1,\"tables\":{\"expanded_vertices\":{\"columns\":[\"sentinel\"],\"rows\":[[\"payload accepted\"]]}},\"mesh\":{}}");
    if (asynchronous) payload.insert(payload.size() - 1, QByteArray(",\"padding\":\"") + QByteArray(64*1024*1024, 'x') + '"');
    if (mode == "payload-root") payload = "[]";
    if (mode == "payload-syntax") payload.chop(2);
    if (mode != "payload-missing") save("geometry.json", payload);
    QVERIFY(QFile::remove(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker));
    bool seen = false, heartbeat = false;
    int ticks = 0;
    qint64 lastTick = 0, maximumGap = 0;
    QElapsedTimer elapsed;
    QTimer pulse;
    QString switchedPath, validationDirectory;
    QObject::connect(&pulse, &QTimer::timeout, &window, [&] {
        if (!window.busy()) return;
        const auto now = elapsed.elapsed();
        maximumGap = std::max(maximumGap, now - lastTick);
        lastTick = now; ++ticks;
    });
    const auto observer = QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window,
        [&](const QString &message) {
            if (seen || message != "Reading report…") return;
            seen = true; elapsed.start();
            QVERIFY(window.busy());
            if (!asynchronous) return;
            pulse.start(5);
            QTimer::singleShot(0, &window, [&] {
                heartbeat = true;
                QVERIFY(window.busy());
                if (mode == "payload-async-destroy") {
                    QFile file(fixture + "/validation-directory.txt");
                    QVERIFY(file.open(QIODevice::ReadOnly));
                    validationDirectory = QString::fromLocal8Bit(file.readAll());
                    QVERIFY(QFileInfo::exists(validationDirectory));
                    owner.reset();
                } else if (mode == "payload-async-close") window.close();
                else if (mode == "payload-async-switch") {
                    QVERIFY(QFile::remove(worker));
                    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
                    switchedPath = QDir(root).filePath("second.gpa_frame");
                    testing::msaaOutputCapture(false).save(switchedPath);
                    window.openCapture(switchedPath);
                } else if (!accepted) {
                    bool cancelled = false;
                    for (auto action : window.findChildren<QAction *>())
                        if (action->shortcut() == QKeySequence(Qt::Key_Escape)) {
                            QVERIFY(action->isEnabled()); action->trigger(); cancelled = true;
                        }
                    QVERIFY(cancelled);
                }
            });
        });
    done.clear(); inspect->trigger();
    if (mode == "payload-async-destroy") {
        QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
        QVERIFY(seen && heartbeat);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000);
        QCOMPARE(done.size(), 0);
        return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), mode == "payload-async-switch" ? 2 : 1, 30000);
    QObject::disconnect(observer);
    pulse.stop();
    QVERIFY(seen);
    if (asynchronous) QVERIFY(heartbeat);
    QCOMPARE(done.first()[0].toBool(), accepted);
    QVERIFY(!window.busy());
    if (mode == "payload-async-switch") {
        QVERIFY(done.last()[0].toBool());
        QCOMPARE(window.capturePath(), switchedPath);
        QCOMPARE(model->rowCount(), 0);
        QTest::qWait(100); QCOMPARE(done.size(), 2);
        return;
    }
    QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(), expectedImage);
    if (accepted) {
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 0).data().toString(), QString("payload accepted"));
    } else {
        QCOMPARE(model->rowCount(), 6);
        QCOMPARE(model->index(0, 0).data(), expectedFirst);
        if (!asynchronous) QVERIFY2(window.statusBar()->currentMessage().contains("geometry.json"), qPrintable(window.statusBar()->currentMessage()));
    }
    if (mode == "payload-async-success") {
        QVERIFY(ticks >= 3);
        qInfo() << "Payload busy heartbeat ticks" << ticks << "maximum_gap_ms" << maximumGap << "completion_ms" << elapsed.elapsed();
    }
    QVERIFY(QFile::remove(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    done.clear(); inspect->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    QCOMPARE(model->rowCount(), 6);
    QCOMPARE(model->index(0, 0).data(), expectedFirst);
    QVERIFY(!window.busy());
    QTest::qWait(100); QCOMPARE(done.size(), 1);
}
