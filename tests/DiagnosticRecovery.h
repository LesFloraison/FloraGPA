#pragma once
#include "DepthStencilCapture.h"
#include "app/CoverageView.h"
#include "app/QuadView.h"

// Every scenario starts with genuine output from the production Worker. Only
// this child's temporary copies are changed for malformed/large-output controls.
inline void exerciseDiagnosticRecovery() {
    using Json = nlohmann::json;
    const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
    const bool quad = mode.startsWith("quad-");
    const auto kind = QString(quad ? "quad" : "coverage");
    const bool asynchronous = mode.contains("-async-");
    const bool accepted = mode.endsWith("-valid") || mode.endsWith("-success");
    const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
    QVERIFY(!QFile::exists(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    const auto capture = QDir(root).filePath("diagnostic.gpa_frame");
    testing::depthStencilCapture().save(capture);
    auto owner = std::make_unique<MainWindow>();
    auto &window = *owner;
    QSignalSpy done(&window, &MainWindow::taskFinished);
    window.openCapture(capture);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    auto selectDraw = [&] {
        const auto api = window.findChild<QTableView *>("apiLog");
        QVERIFY(api);
        for (int row = 0; row < api->model()->rowCount(); ++row) {
            auto index = api->model()->index(row, 0);
            if (index.data(Qt::UserRole).toULongLong() == 1000) {
                api->setCurrentIndex(index); return;
            }
        }
        QFAIL("Diagnostic Draw is missing");
    };
    selectDraw();
    auto inspect = window.findChild<QAction *>(quad ? "captureQuad" : "captureCoverage");
    QVERIFY(inspect && inspect->isEnabled());
    done.clear(); inspect->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
    auto image = window.findChild<ImageView *>(quad ? "quadImage" : "coverageImage");
    QVERIFY(image && !image->image().isNull());
    const auto expected = image->image();
    const auto expectedFrame = window.findChild<ImageView *>("frameOutput")->image();
    const auto fixture = QDir(root).filePath("fault-output");
    qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
    QProcess producer;
    producer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    producer.start(worker, {kind, capture, "--id", "1000", "--out", fixture});
    QVERIFY(producer.waitForStarted());
    const auto stopProducer = qScopeGuard([&] { if (producer.state() != QProcess::NotRunning) { producer.kill(); producer.waitForFinished(); } });
    QTRY_COMPARE_WITH_TIMEOUT(producer.state(), QProcess::NotRunning, 30000);
    QCOMPARE(producer.exitStatus(), QProcess::NormalExit);
    QVERIFY2(producer.exitCode() == 0, producer.readAllStandardError().constData());
    auto save = [&](const QString &name, const QByteArray &bytes) {
        QFile file(fixture + '/' + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    };
    QFile reportFile(fixture + '/' + kind + ".json");
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    auto report = Json::parse(reportFile.readAll().toStdString()); reportFile.close();
    const auto preview = QString(quad ? "data/quad_counts.png" : "overlay.png");
    // Retain realistic report fields and untouched attachments for small cases.
    // The larger dimensions are explicitly synthetic analyzer-output stress,
    // not evidence that the one-pixel source capture produces such a target.
    const auto size = asynchronous ? QSize(4096, 4096) : expected.size();
    QImage sentinel(size, QImage::Format_RGBA8888);
    sentinel.fill(QColor(53, 97, 179, 211));
    QVERIFY(sentinel.save(fixture + '/' + preview));
    if (asynchronous && quad) {
        report["quad_width"] = size.width(); report["quad_height"] = size.height();
        report["width"] = size.width() * 2; report["height"] = size.height() * 2;
        const QByteArray cells(qsizetype(size.width()) * size.height() * 4, 0);
        for (const auto name : {"data/locks.u32le", "data/counts.u32le", "data/live.u32le"}) save(name, cells);
    } else if (asynchronous) {
        auto &shape = report.at("target_subresource");
        shape["width"] = size.width(); shape["height"] = size.height();
        QVERIFY(sentinel.save(fixture + "/coverage.png"));
        QVERIFY(sentinel.save(fixture + "/after_draw.png"));
    }
    if (mode.endsWith("-missing")) QVERIFY(QFile::remove(fixture + '/' + preview));
    if (mode.endsWith("-corrupt")) save(preview, "not a PNG");
    if (mode.endsWith("-dimensions")) {
        QImage wrong(size.width() + 1, size.height(), QImage::Format_RGBA8888);
        wrong.fill(Qt::red); QVERIFY(wrong.save(fixture + '/' + preview));
    }
    if (mode.endsWith("-event")) {
        if (quad) report["event_id"] = 2000; else report["event"]["id"] = 2000;
    }
    if (mode == "quad-overflow") {
        report["histogram_capacity"] = uint64_t(1) << 62;
        save("data/histogram.u32le", {});
    }
    if (mode == "quad-short" || mode == "quad-long") {
        const auto length = qsizetype(size.width()) * size.height() * 4 + (mode == "quad-short" ? -1 : 1);
        save("data/counts.u32le", QByteArray(length, 0));
    }
    save(kind + ".json", QByteArray::fromStdString(report.dump()));
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
                if (mode.endsWith("-destroy")) {
                    QFile file(fixture + "/validation-directory.txt");
                    QVERIFY(file.open(QIODevice::ReadOnly));
                    validationDirectory = QString::fromLocal8Bit(file.readAll());
                    QVERIFY(QFileInfo::exists(validationDirectory)); owner.reset();
                } else if (mode.endsWith("-close")) window.close();
                else if (mode.endsWith("-switch")) {
                    QVERIFY(QFile::remove(worker));
                    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
                    switchedPath = QDir(root).filePath("second.gpa_frame");
                    testing::depthStencilCapture().save(switchedPath);
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
    if (mode.endsWith("-destroy")) {
        QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
        QVERIFY(seen && heartbeat);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000);
        QCOMPARE(done.size(), 0);
        return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), mode.endsWith("-switch") ? 2 : 1, 30000);
    QObject::disconnect(observer); pulse.stop();
    QVERIFY(seen);
    if (asynchronous) QVERIFY(heartbeat);
    QCOMPARE(done.first()[0].toBool(), accepted);
    QVERIFY(!window.busy());
    if (mode.endsWith("-switch")) {
        QVERIFY(done.last()[0].toBool());
        QCOMPARE(window.capturePath(), switchedPath);
    } else {
        QCOMPARE(window.capturePath(), capture);
        QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(), expectedFrame);
    }
    if (accepted) {
        QCOMPARE(image->image().convertToFormat(QImage::Format_RGBA8888), sentinel);
        // Keep original alpha for inspection while preserving the old RGB paint.
        QCOMPARE(image->displayImage().pixelColor(0, 0), QColor(53, 97, 179));
        QCOMPARE(image->displayImage().pixelColor(size.width() - 1, size.height() - 1), QColor(53, 97, 179));
    } else {
        QVERIFY(image->image().isNull()); // A new diagnostic clears its previous result.
        if (!asynchronous) {
            const auto error = window.statusBar()->currentMessage();
            const QString detail = mode.endsWith("-event") ? QString("event does not match") :
                                   mode == "quad-overflow" ? QString("histogram byte length overflows") :
                                   mode == "quad-short" || mode == "quad-long" ? QString("counts.u32le") : QFileInfo(preview).fileName();
            QVERIFY2(error.contains(detail, Qt::CaseInsensitive), qPrintable(error));
            if (!mode.endsWith("-event")) QVERIFY(error.contains(kind + ".json"));
            qInfo().noquote() << "diagnostic rejection" << mode << error;
        }
    }
    if (mode.endsWith("-success")) {
        QVERIFY(ticks >= 3);
        qInfo() << "Diagnostic busy heartbeat" << kind << "ticks" << ticks << "maximum_gap_ms" << maximumGap << "completion_ms" << elapsed.elapsed();
    }
    const int completions = done.size();
    QTest::qWait(100); QCOMPARE(done.size(), completions);
    QVERIFY(QFile::remove(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    if (!switchedPath.isEmpty()) selectDraw();
    QVERIFY(inspect->isEnabled());
    done.clear(); inspect->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
    QCOMPARE(image->image(), expected);
    QVERIFY(!window.busy());
    QTest::qWait(100); QCOMPARE(done.size(), 1);
}
