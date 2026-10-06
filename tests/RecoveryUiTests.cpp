#include "MsaaCapture.h"
#include "app/CompatibilityButton.h"
#include "app/MainWindow.h"
#include <QAction>
#include <QSignalSpy>
#include <QStatusBar>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <Psapi.h>
#include <QtTest>

using namespace flora;
class RecoveryUiTests final : public QObject {
    Q_OBJECT
    QAction *cancelAction(MainWindow &window) {
        for (auto action : window.findChildren<QAction *>())
            if (action->shortcut() == QKeySequence(Qt::Key_Escape))
                return action;
        return nullptr;
    }
    QByteArray outputHash(MainWindow &window) {
        auto view = window.findChild<ImageView *>("frameOutput");
        if (!view || view->image().isNull())
            return {};
        const auto rgba = view->image().convertToFormat(QImage::Format_RGBA8888);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (int y = 0; y < rgba.height(); ++y)
            hash.addData(QByteArrayView(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4));
        return hash.result().toHex();
    }
  private slots:
    void cancelOpenThenRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        testing::msaaOutputCapture(false).save(path);
        MainWindow window;
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        QSignalSpy done(&window, &MainWindow::taskFinished);
        window.openCapture(path);
        auto cancel = cancelAction(window);
        QVERIFY(cancel);
        QVERIFY(cancel->isEnabled());
        cancel->trigger(); // Also covers completion already queued before cancellation.
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
        QVERIFY(!done.last()[0].toBool());
        QCOMPARE(loaded.size(), 0);
        QVERIFY(window.capturePath().isEmpty());
        QVERIFY(!window.busy());
        QCOMPARE(window.statusBar()->currentMessage(), QString("Opening cancelled."));
        done.clear();
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.last()[0].toBool());
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(window.capturePath(), path);
        QVERIFY(!window.busy());
    }
    void failedOpenPreservesCaptureAndPreflight() {
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        testing::msaaOutputCapture(false).save(path);
        const auto broken = dir.filePath("broken.gpa_frame");
        QFile file(broken);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("IGPA");
        file.close();
        MainWindow window;
        QSignalSpy done(&window, &MainWindow::taskFinished);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
        QVERIFY(done.last()[0].toBool());
        auto preflight = window.findChild<CompatibilityButton *>();
        QVERIFY(preflight);
        QSignalSpy checked(preflight, &CompatibilityButton::resultReady);
        preflight->click();
        QTRY_COMPARE_WITH_TIMEOUT(checked.size(), 1, 10000);
        const auto report = preflight->report();
        QVERIFY(!report.is_null());
        for (int repeat = 0; repeat < 3; ++repeat) {
            done.clear();
            window.openCapture(broken);
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
            QVERIFY(!done.last()[0].toBool());
            QCOMPARE(window.statusBar()->currentMessage(), QString("Capture header is truncated"));
            QCOMPARE(window.capturePath(), path);
            QCOMPARE(preflight->report(), report);
            QCOMPARE(loaded.size(), 1);
            QVERIFY(!window.busy());
            done.clear();
            window.openCapture(path);
            cancelAction(window)->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
            QVERIFY(!done.last()[0].toBool());
            QCOMPARE(preflight->report(), report);
            QCOMPARE(loaded.size(), 1);
            done.clear();
            window.replay();
            QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 30000);
            QVERIFY(done.last()[0].toBool());
        }
    }
    void closeDuringOpen() {
        QTemporaryDir dir;
        const auto path = dir.filePath("large.gpa_frame");
        testing::Capture capture;
        capture.save(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(256 * 1024 * 1024));
        file.close();
        QElapsedTimer timer;
        timer.start();
        {
            MainWindow window;
            window.openCapture(path);
            // Destruction must signal the task before waiting for it.
        }
        QVERIFY2(timer.elapsed() < 5000, "Closing waited for the complete file hash");
        QVERIFY(file.open(QIODevice::ReadWrite));
    }
    void originalCaptureRecovery() {
        const auto root = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set FLORA_TEST_CAPTURE_DIR for original GF2/BF1 recovery checks");
        MainWindow window;
        QSignalSpy done(&window, &MainWindow::taskFinished);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        const QStringList files{"GF2_Exilium_2026_03_03__00_19_35.gpa_frame", "bf1_2026_01_21__16_53_05.gpa_frame"};
        const int iterations = qEnvironmentVariableIntValue("FLORA_RECOVERY_ITERATIONS");
        const int count = iterations > 0 ? iterations : 2;
        for (int repeat = 0; repeat < count; ++repeat) {
            for (const auto &name : files) {
                QElapsedTimer elapsed;
                elapsed.start();
                const auto path = root + '/' + name;
                const QByteArray golden = name.startsWith("GF2")
                    ? "2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"
                    : "1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6";
                QVERIFY(QFile::exists(path));
                const auto previous = window.capturePath();
                const auto before = loaded.size();
                done.clear();
                window.openCapture(path);
                QVERIFY(window.busy());
                cancelAction(window)->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
                QVERIFY(!done.last()[0].toBool());
                QCOMPARE(window.capturePath(), previous);
                QCOMPARE(loaded.size(), before);
                done.clear();
                window.openCapture(path);
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
                QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                QCOMPARE(loaded.size(), before + 1);
                QVERIFY(!window.busy());
                QCOMPARE(window.capturePath(), path);
                QCOMPARE(outputHash(window), golden);
                done.clear();
                window.replay();
                cancelAction(window)->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
                QVERIFY(!done.last()[0].toBool());
                QVERIFY(!window.busy());
                done.clear();
                auto api = window.findChild<QTableView *>("apiLog");
                QVERIFY(api && api->model()->rowCount() > 0);
                api->setCurrentIndex(api->model()->index(0, 0));
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
                QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                QVERIFY(!window.busy());
                // Selection intentionally switches to After Event. Return to Final
                // before comparing with the independently pinned full-frame golden.
                done.clear();
                auto boundary = window.findChild<QComboBox *>("outputBoundary");
                QVERIFY(boundary);
                QCOMPARE(boundary->currentIndex(), 2);
                boundary->setCurrentIndex(0);
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
                QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                QCOMPARE(outputHash(window), golden);
                PROCESS_MEMORY_COUNTERS_EX memory{};
                DWORD handles{};
                QVERIFY(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof memory));
                QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &handles));
                const nlohmann::json observation{{"iteration", repeat}, {"capture", name.toStdString()},
                    {"elapsed_ms", elapsed.elapsed()}, {"private_bytes", memory.PrivateUsage},
                    {"working_set", memory.WorkingSetSize}, {"handles", handles},
                    {"gdi_objects", GetGuiResources(GetCurrentProcess(), 0)},
                    {"user_objects", GetGuiResources(GetCurrentProcess(), 1)},
                    {"rgba_sha256", golden.toStdString()}};
                qInfo().noquote() << "recovery_observation" << QString::fromStdString(observation.dump());
            }
        }
        qInfo() << "Original recovery cycles:" << count * files.size();
    }
};
QTEST_MAIN(RecoveryUiTests)
#include "RecoveryUiTests.moc"
