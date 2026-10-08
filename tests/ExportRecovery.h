#pragma once
#include <QFileDialog>
#include <QLineEdit>

inline void exerciseExportRecovery() {
    using Json = nlohmann::json;
    const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only in the isolated executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
    const bool asynchronous = mode.contains("-async-");
    const bool accepted = mode.endsWith("-valid") || mode.endsWith("-success");
    const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
    const auto capture = QDir(root).filePath("export.gpa_frame");
    testing::msaaOutputCapture(false).save(capture);
    const auto fixture = QDir(root).filePath("fault-output");
    QProcess producer;
    producer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    producer.start(qEnvironmentVariable("FLORA_REAL_WORKER"), {"replay", capture, "--out", fixture});
    QVERIFY(producer.waitForStarted());
    QTRY_COMPARE_WITH_TIMEOUT(producer.state(), QProcess::NotRunning, 30000);
    QCOMPARE(producer.exitCode(), 0);
    auto read = [](const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read export fixture");
        return file.readAll();
    };
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            throw std::runtime_error("Cannot write export fixture");
    };
    const auto raw = fixture + "/output_storage.bin", metadata = fixture + "/output_storage.json";
    auto report = Json::parse(read(fixture + "/report.json").toStdString());
    const auto original = read(raw), originalMetadata = read(metadata);
    if (asynchronous) {
        // Stress the exporter only. This synthetic Worker sidecar is not a new
        // captured-resource semantic claim; the real replay below remains small.
        const QByteArray large(64 * 1024 * 1024, 'x');
        write(raw, large);
        report["output_display"]["storage_bytes"] = large.size();
        report["output_display"]["storage_sha256"] = QCryptographicHash::hash(large, QCryptographicHash::Sha256).toHex().toStdString();
        write(metadata, QByteArray::fromStdString(report["output_display"].dump(2) + "\n"));
        write(fixture + "/report.json", QByteArray::fromStdString(report.dump()));
    }
    if (mode == "export-short") write(raw, original.chopped(1));
    if (mode == "export-long") write(raw, original + "x");
    if (mode == "export-hash") { auto bad = original; bad[0] ^= char(1); write(raw, bad); }
    if (mode == "export-metadata") {
        auto bad = report["output_display"]; bad["resource"] = 21;
        write(metadata, QByteArray::fromStdString(bad.dump(2) + "\n"));
    }
    if (mode == "export-missing-metadata") QVERIFY(QFile::remove(metadata));
    qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
    QVERIFY(!QFile::exists(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker));
    auto owner = std::make_unique<MainWindow>(); auto &window = *owner;
    window.show();
    QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
    window.openCapture(capture);
    QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000);
    QVERIFY(done.last()[0].toBool());
    const auto image = window.findChild<ImageView *>("frameOutput")->image();
    const auto path = QDir(root).filePath("saved.bin");
    write(path, "Old binary"); write(path + ".json", "Old metadata");
    const bool native = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    auto restore = qScopeGuard([&] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native); });
    auto action = window.findChild<QAction *>("exportOutputStorage"); QVERIFY(action && action->isEnabled());
    auto choose = [&] {
        QTimer chooser, watchdog;
        bool seen = false;
        watchdog.setSingleShot(true);
        QObject::connect(&chooser, &QTimer::timeout, &window, [&] {
            auto dialog = window.findChild<QFileDialog *>(); if (!dialog) return;
            chooser.stop(); seen = true;
            // Existing targets are deliberate preservation controls.
            dialog->setOption(QFileDialog::DontConfirmOverwrite);
            auto filename = dialog->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(filename);
            filename->setText(path); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        QObject::connect(&watchdog, &QTimer::timeout, &window, [&] {
            if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject();
        });
        chooser.start(10); watchdog.start(30000); action->trigger(); QVERIFY(seen);
    };
    bool seen = false, heartbeat = false;
    int ticks = 0;
    QTimer pulse;
    QObject::connect(&pulse, &QTimer::timeout, &window, [&] { if (window.busy()) ++ticks; });
    QString validationDirectory, switched;
    auto observer = QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &text) {
        if (seen || text != "Exporting output storage…") return;
        seen = true; QVERIFY(window.busy());
        if (!asynchronous) return;
        pulse.start(5);
        QTimer::singleShot(0, &window, [&] {
            heartbeat = true; QVERIFY(window.busy());
            if (accepted) return;
            if (mode.endsWith("-destroy")) {
                validationDirectory = QString::fromLocal8Bit(read(fixture + "/validation-directory.txt")); owner.reset();
            } else if (mode.endsWith("-close")) window.close();
            else if (mode.endsWith("-switch")) {
                QVERIFY(QFile::remove(worker)); QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
                switched = QDir(root).filePath("second.gpa_frame"); testing::msaaOutputCapture(false).save(switched);
                done.clear(); window.openCapture(switched);
            } else {
                auto cancel = window.findChild<QAction *>("cancelTask");
                if (!cancel) for (auto candidate : window.findChildren<QAction *>()) if (candidate->text() == "Cancel") cancel = candidate;
                QVERIFY(cancel && cancel->isEnabled()); cancel->trigger();
            }
        });
    });
    choose();
    if (mode.endsWith("-destroy")) {
        QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000); QVERIFY(seen && heartbeat);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000);
        QCOMPARE(exported.size(), 0); QCOMPARE(read(path), QByteArray("Old binary"));
        QCOMPARE(read(path + ".json"), QByteArray("Old metadata")); return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
    QObject::disconnect(observer); pulse.stop();
    QVERIFY(seen); if (asynchronous) QVERIFY(heartbeat);
    QCOMPARE(exported.last()[0].toBool(), accepted);
    if (accepted) { QCOMPARE(read(path), read(raw)); QCOMPARE(read(path + ".json"), read(metadata)); }
    else { QCOMPARE(read(path), QByteArray("Old binary")); QCOMPARE(read(path + ".json"), QByteArray("Old metadata")); }
    if (mode.endsWith("-success")) { QVERIFY(ticks >= 1); qInfo() << "Export heartbeat ticks" << ticks; }
    QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(), image);
    if (!accepted && !asynchronous) QVERIFY(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText().contains("Output storage"));
    if (!switched.isEmpty()) { QCOMPARE(window.capturePath(), switched); QVERIFY(!done.empty() && done.last()[0].toBool()); }
    // The same UI action succeeds after a real worker replay replaces bad data.
    QVERIFY(QFile::remove(worker)); QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    window.show(); done.clear(); window.replay();
    QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
    exported.clear(); QVERIFY(action->isEnabled()); choose();
    QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
    QCOMPARE(read(path), original); QCOMPARE(read(path + ".json"), originalMetadata);
}
