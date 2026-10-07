#pragma once
#include "app/IntelMetricsView.h"
#include <QTabWidget>
#include <QLineEdit>
#include <QTreeWidget>

inline void exerciseCatalogRecovery() {
    using Json = nlohmann::json;
    const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only in the isolated executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE");
    const bool asynchronous = mode.contains("-async-");
    const bool accepted = mode.endsWith("-valid") || mode.endsWith("-success");
    const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
    QVERIFY(!QFile::exists(worker));
    QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    const auto capture = QDir(root).filePath("catalog.gpa_frame");
    testing::msaaOutputCapture(false).save(capture);
    auto owner = std::make_unique<MainWindow>(); auto &window = *owner;
    window.show();
    QSignalSpy done(&window, &MainWindow::taskFinished);
    window.openCapture(capture);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 30000);
    QVERIFY(done.last()[0].toBool());
    const auto image = window.findChild<ImageView *>("frameOutput")->image();
    auto intel = window.findChild<IntelMetricsView *>(); QVERIFY(intel);
    auto scheduled = intel->findChild<ScheduledMetricsView *>(); QVERIFY(scheduled);
    const auto bridge = QDir(root).filePath("FloraGPA.Metrics.dll");
    QVERIFY(QFileInfo::exists(bridge));
    Json catalog{{"sets", {{{"name", "OldSet"}, {"metrics", {{{"name", "OldMetric"}, {"label", "Old metric"}, {"unit", "ns"}}}}}}}};
    for (auto view : intel->findChildren<UniformMetricsView *>()) view->setCatalog(catalog, bridge);
    scheduled->setCatalog(catalog, bridge);
    intel->findChild<QComboBox *>("intelMetricMode")->setCurrentIndex(2);
    auto settings = scheduled->settings(); settings["bridge"] = bridge.toStdString(); scheduled->restoreSettings(settings);
    auto catalogs = [&] {
        QStringList rows;
        for (auto tree : intel->findChildren<QTreeWidget *>())
            if (tree->objectName() == "uniformAvailable" || tree->objectName() == "scheduledAvailable") {
                rows << tree->objectName();
                for (int i = 0; i < tree->topLevelItemCount(); ++i)
                    rows << tree->topLevelItem(i)->text(0) << tree->topLevelItem(i)->text(1);
            }
        return rows;
    };
    const auto original = catalogs();
    catalog["sets"][0]["name"] = "NewSet";
    catalog["sets"][0]["metrics"][0]["name"] = "NewMetric";
    const auto good = catalog;
    const auto fixture = QDir(root).filePath("fault-output");
    QVERIFY(QDir().mkpath(fixture));
    qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
    auto write = [&](const Json &value) {
        QFile file(fixture + "/catalog.json"); QVERIFY(file.open(QIODevice::WriteOnly));
        auto bytes = QByteArray::fromStdString(value.dump());
        if (mode.endsWith("-syntax")) bytes.chop(2);
        QCOMPARE(file.write(bytes), bytes.size());
    };
    if (mode.endsWith("-late-invalid")) catalog["sets"][0]["metrics"].push_back({{"name", 42}, {"unit", "ns"}});
    if (mode.endsWith("-root")) catalog = Json::array();
    if (asynchronous) catalog["padding"] = std::string(64 * 1024 * 1024, 'x');
    if (!mode.endsWith("-missing")) write(catalog);
    QVERIFY(QFile::remove(worker)); QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker));
    bool seen = false, heartbeat = false; int ticks = 0;
    QTimer pulse; QElapsedTimer elapsed; qint64 previous = 0, gap = 0;
    QString switched, validationDirectory;
    QObject::connect(&pulse, &QTimer::timeout, &window, [&] {
        if (window.busy()) { auto now = elapsed.elapsed(); gap = std::max(gap, now-previous); previous = now; ++ticks; }
    });
    auto observer = QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &text) {
        if (seen || text != "Reading report…") return;
        seen = true; QVERIFY(window.busy()); elapsed.start();
        if (!asynchronous) return;
        pulse.start(5);
        QTimer::singleShot(0, &window, [&] {
            heartbeat = true; QVERIFY(window.busy());
            if (accepted) return;
            if (mode.endsWith("-destroy")) {
                QFile file(fixture + "/validation-directory.txt"); QVERIFY(file.open(QIODevice::ReadOnly));
                validationDirectory = QString::fromLocal8Bit(file.readAll()); owner.reset();
            } else if (mode.endsWith("-close")) window.close();
            else if (mode.endsWith("-switch")) {
                QVERIFY(QFile::remove(worker)); QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
                switched = QDir(root).filePath("second.gpa_frame"); testing::msaaOutputCapture(false).save(switched); window.openCapture(switched);
            } else {
                auto cancel = scheduled->findChild<QAction *>("scheduledCancel"); QVERIFY(cancel && cancel->isEnabled()); cancel->trigger();
            }
        });
    });
    done.clear();
    auto action = scheduled->findChild<QAction *>("scheduledReadCatalog"); QVERIFY(action && action->isEnabled()); action->trigger();
    if (mode.endsWith("-destroy")) {
        QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000); QVERIFY(seen && heartbeat);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(validationDirectory), 30000); QCOMPARE(done.size(), 0); return;
    }
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !done.empty(), 30000);
    QObject::disconnect(observer); pulse.stop();
    if (!accepted) QCOMPARE(catalogs(), original); // Late malformed rows must not partially replace a catalog.
    else { QVERIFY(catalogs().contains("NewMetric")); QVERIFY(!catalogs().contains("OldMetric")); }
    QVERIFY(seen);
    if (asynchronous) QVERIFY(heartbeat);
    QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(), image);
    const int completions = switched.isEmpty() ? 1 : 2;
    QCOMPARE(done.size(), completions);
    if (!switched.isEmpty()) { QVERIFY(!done.first()[0].toBool()); QCOMPARE(window.capturePath(), switched); }
    QCOMPARE(done.last()[0].toBool(), accepted || !switched.isEmpty());
    if (!accepted && !asynchronous) QVERIFY(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText().contains("catalog.json"));
    if (mode.endsWith("-success")) { QVERIFY(ticks >= 3); qInfo() << "Catalog heartbeat ticks" << ticks << "maximum_gap_ms" << gap; }
    QTest::qWait(100); QCOMPARE(done.size(), completions);
    // Repair and retry the actual catalog UI request; keep production replay independent.
    { QFile file(fixture + "/catalog.json"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QByteArray::fromStdString(good.dump())); }
    if (!switched.isEmpty()) { QVERIFY(QFile::remove(worker)); QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_FAULT_WORKER"), worker)); }
    window.show(); done.clear(); QVERIFY(action->isEnabled()); action->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !done.empty(), 30000); QVERIFY(done.last()[0].toBool());
    QVERIFY(catalogs().contains("NewMetric")); QVERIFY(!catalogs().contains("OldMetric"));
    QVERIFY(QFile::remove(worker)); QVERIFY(QFile::copy(qEnvironmentVariable("FLORA_REAL_WORKER"), worker));
    done.clear(); window.replay();
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy() && !done.empty(), 30000); QVERIFY(done.last()[0].toBool());
    QCOMPARE(window.findChild<ImageView *>("frameOutput")->image(), image);
}
