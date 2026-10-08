#pragma once
#include "StreamCapture.h"
#include "app/GeometryExport.h"
#include <QFileDialog>
#include <QThreadPool>
inline void exerciseGeometryExportRecovery() {
    const auto root = qEnvironmentVariable("FLORA_FAULT_ROOT");
    if (root.isEmpty()) QSKIP("Runs only inside the parent-owned temporary executable directory");
    QCOMPARE(QFileInfo(root).canonicalFilePath(), QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath());
    const auto mode = qEnvironmentVariable("FLORA_FAULT_MODE").mid(QString("geometry-export-").size());
    const auto worker = QDir(root).filePath("FloraGPA.Worker.exe");
    const auto capture = QDir(root).filePath("stream.gpa_frame"), replacement = QDir(root).filePath("second.gpa_frame");
    flora::testing::streamCapture().save(capture);
    flora::testing::msaaOutputCapture(false).save(replacement);
    const auto fixture = QDir(root).filePath("fixture"), destination = QDir(root).filePath("exports");
    QProcess producer;
    producer.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    producer.start(qEnvironmentVariable("FLORA_REAL_WORKER"), {"geometry", capture, "--event", "200", "--out", fixture});
    QVERIFY(producer.waitForStarted());
    QTRY_COMPARE_WITH_TIMEOUT(producer.state(), QProcess::NotRunning, 30000);
    QCOMPARE(producer.exitCode(), 0);
    const auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
    };
    const auto read = [](const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read export fixture");
        return file.readAll();
    };
    // An additional supported geometry asset stresses copying only; it does not
    // claim that this small IA fixture produces 64 MiB of replay vertex data.
    write(fixture + "/vertices.bin", QByteArray(64 * 1024 * 1024, 'g'));
    const auto assets = flora::geometryExportAssets(fixture);
    QVERIFY(QDir().mkpath(destination + "/FloraGPA-Geometry-200"));
    write(destination + "/FloraGPA-Geometry-200/keep", "Previous export");
    qputenv("FLORA_FAULT_IMAGE_ROOT", fixture.toUtf8());
    auto owner = std::make_unique<flora::MainWindow>(); auto &window = *owner;
    window.show();
    QSignalSpy done(&window, &flora::MainWindow::taskFinished), exported(&window, &flora::MainWindow::exportFinished);
    const auto replaceWorker = [&](const QString &path) {
        if (QFile::exists(worker)) QVERIFY(QFile::remove(worker));
        QVERIFY(QFile::copy(path, worker));
    };
    QString cache;
    const auto prepare = [&] {
        replaceWorker(qEnvironmentVariable("FLORA_REAL_WORKER"));
        window.show(); done.clear(); window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        auto api = window.findChild<QTableView *>("apiLog");
        done.clear(); api->setCurrentIndex(api->model()->index(2, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        replaceWorker(qEnvironmentVariable("FLORA_FAULT_WORKER"));
        done.clear(); window.findChild<QAction *>("inspectGeometry")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        cache = QString::fromLocal8Bit(read(fixture + "/validation-directory.txt"));
        QVERIFY(QFileInfo::exists(cache + "/geometry.json"));
    };
    prepare();
    const bool native = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    auto restore = qScopeGuard([&] { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native); });
    HANDLE lock = INVALID_HANDLE_VALUE;
    auto release = qScopeGuard([&] { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); });
    bool first = true;
    const auto choose = [&] {
        QTimer chooser, watchdog; bool seen = false;
        watchdog.setSingleShot(true);
        QObject::connect(&chooser, &QTimer::timeout, &chooser, [&] {
            auto dialog = window.findChild<QFileDialog *>(); if (!dialog) return;
            chooser.stop(); seen = true;
            if (first && mode == "missing") QVERIFY(QFile::remove(cache + "/vertices.bin"));
            if (first && mode == "grow") { QFile file(cache + "/vertices.bin"); QVERIFY(file.open(QIODevice::Append)); QCOMPARE(file.write("x"), qint64(1)); }
            if (first && mode == "source-lock") {
                lock = CreateFileW(reinterpret_cast<const wchar_t *>((cache + "/vertices.bin").utf16()), GENERIC_READ,
                    0, nullptr, OPEN_EXISTING, 0, nullptr); QVERIFY(lock != INVALID_HANDLE_VALUE);
            }
            auto filename = dialog->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(filename); filename->setText(destination);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] { if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject(); });
        chooser.start(10); watchdog.start(30000);
        window.findChild<QAction *>("exportGeometry")->trigger();
        QVERIFY(seen);
    };
    bool seen = false, heartbeat = false; int ticks = 0;
    QTimer pulse;
    QObject::connect(&pulse, &QTimer::timeout, &pulse, [&] { if (owner && owner->busy()) ++ticks; });
    const auto observer = QObject::connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &message) {
        if (seen || message != "Exporting geometry…") return;
        seen = true; QVERIFY(window.busy()); pulse.start(1);
        QTimer::singleShot(0, &window, [&] {
            heartbeat = true;
            if (mode == "destroy") owner.reset();
            else if (mode == "close") window.close();
            else if (mode == "switch") { replaceWorker(qEnvironmentVariable("FLORA_REAL_WORKER")); done.clear(); window.openCapture(replacement); }
            else if (mode == "cancel") {
                QAction *cancel = nullptr;
                for (auto action : window.findChildren<QAction *>()) if (action->text() == "Cancel") cancel = action;
                QVERIFY(cancel && cancel->isEnabled()); cancel->trigger();
            }
        });
    });
    choose();
    const auto dirs = [&] { return QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden); };
    if (mode == "destroy") {
        QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(), 0, 30000);
        QVERIFY(seen && heartbeat); QCOMPARE(exported.size(), 0);
        QCOMPARE(dirs(), QStringList{"FloraGPA-Geometry-200"});
        QVERIFY(!QFileInfo::exists(QFileInfo(cache).absolutePath())); return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
    QObject::disconnect(observer); pulse.stop();
    QVERIFY(seen && heartbeat); QCOMPARE(exported.last()[0].toBool(), mode == "success");
    QCOMPARE(read(destination + "/FloraGPA-Geometry-200/keep"), QByteArray("Previous export"));
    if (mode == "success") {
        QCOMPARE(dirs().size(), 2); QVERIFY(ticks > 0); qInfo() << "Geometry export UI ticks:" << ticks;
        for (const auto &asset : assets) QCOMPARE(read(destination + "/FloraGPA-Geometry-200-1/" + asset.name), read(fixture + '/' + asset.name));
    } else QCOMPARE(dirs(), QStringList{"FloraGPA-Geometry-200"});
    if (lock != INVALID_HANDLE_VALUE) { CloseHandle(lock); lock = INVALID_HANDLE_VALUE; }
    first = false; prepare(); exported.clear(); choose();
    QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
    const auto path = destination + (mode == "success" ? "/FloraGPA-Geometry-200-2/" : "/FloraGPA-Geometry-200-1/");
    for (const auto &asset : assets) QCOMPARE(read(path + asset.name), read(fixture + '/' + asset.name));
}
