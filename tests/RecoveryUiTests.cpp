#include "MsaaCapture.h"
#include "app/CompatibilityButton.h"
#include "app/MainWindow.h"
#include "HeapRetentionProbe.h"
#include "RecoveryJournal.h"
#include "RecoveryWorkflows.h"
#include "ProcessMemorySnapshot.h"
#include <QAction>
#include <QSignalSpy>
#include <QStatusBar>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <Psapi.h>
#include <TlHelp32.h>
#include <QScopeGuard>
#include <QSaveFile>
#include <QDateTime>
#include <QPixmapCache>
#include <QWindow>

// Observe the test executable's import used by the statically linked FloraUi.
// No production binary on disk or other process is modified.
extern "C" BOOL (WINAPI *__imp_CloseHandle)(HANDLE);
static BOOL (WINAPI *nativeCloseHandle)(HANDLE) = nullptr;
static volatile LONG failedCloseCalls = 0;
static BOOL WINAPI observedCloseHandle(HANDLE handle) {
    const BOOL result = nativeCloseHandle(handle);
    const auto error = GetLastError();
    if (!result && error == ERROR_INVALID_HANDLE)
        InterlockedIncrement(&failedCloseCalls);
    SetLastError(error);
    return result;
}
#include <QtTest>

using namespace flora;
class RecoveryUiTests final : public QObject {
    Q_OBJECT
    nlohmann::json ownership(MainWindow &window) {
        nlohmann::json classes = nlohmann::json::object();
        for (auto object : window.findChildren<QObject *>()) {
            auto &count = classes[object->metaObject()->className()];
            count = count.is_null() ? 1 : count.get<unsigned>() + 1;
        }
        auto log = window.findChild<QPlainTextEdit *>("taskLog");
        nlohmann::json result{{"qobjects", classes},
            {"log_blocks", log ? log->document()->blockCount() : 0},
            {"log_characters", log ? log->document()->characterCount() : 0}};
        if (!qEnvironmentVariableIsSet("FLORA_RECOVERY_HEAP"))
            return result;
        // Test-only snapshot: no allocation or Qt calls while a heap is locked.
        std::vector<HANDLE> heaps(GetProcessHeaps(0, nullptr) + 32);
        const auto count = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
        if (count > heaps.size()) {
            result["heap_walk_complete"] = false;
            return result;
        }
        uint64_t bytes = 0, blocks = 0;
        bool complete = true;
        for (DWORD i = 0; i < count; ++i) {
            if (!HeapLock(heaps[i])) { complete = false; continue; }
            PROCESS_HEAP_ENTRY entry{};
            while (HeapWalk(heaps[i], &entry))
                if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                    bytes += entry.cbData;
                    ++blocks;
                }
            const auto error = GetLastError();
            HeapUnlock(heaps[i]);
            complete = complete && error == ERROR_NO_MORE_ITEMS;
        }
        result["heap_walk_complete"] = complete;
        result["heap_busy_bytes"] = bytes;
        result["heap_busy_blocks"] = blocks;
        result["heap_count"] = count;
        return result;
    }
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
    void journalSnapshotsRemainReadable() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("journal.json");
        testing::RecoveryJournal writer(path);
        nlohmann::json state{{"completed", false}, {"phase", "starting"},
                             {"observations", nlohmann::json::array()}};
        QVERIFY2(writer.save(state), qPrintable(writer.error()));
        const auto first = path + ".progress/00000001.json";
        const HANDLE reader = CreateFileW(reinterpret_cast<LPCWSTR>(first.utf16()), GENERIC_READ,
                                          FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(reader != INVALID_HANDLE_VALUE);
        const auto close = qScopeGuard([&] { CloseHandle(reader); });
        // This deliberately restrictive observer reproduces replacement failure.
        QSaveFile overwrite(first);
        QVERIFY(overwrite.open(QIODevice::WriteOnly));
        QCOMPARE(overwrite.write("changed"), qint64(7));
        QVERIFY(!overwrite.commit());
        qInfo().noquote() << "Locked replacement negative control:" << overwrite.errorString();
        state["phase"] = "cycle_complete";
        state["observations"].push_back({{"iteration", 0}, {"rgba_sha256", "checked"}});
        QVERIFY2(writer.save(state), qPrintable(writer.error()));
        QVERIFY(QFileInfo::exists(path + ".progress/00000002.json"));
        state["completed"] = true;
        QVERIFY2(writer.save(state), qPrintable(writer.error()));
        QFile final(path);
        QVERIFY(final.open(QIODevice::ReadOnly));
        QCOMPARE(nlohmann::json::parse(final.readAll().toStdString()), state);
        QVERIFY(!writer.save(state));
        testing::RecoveryJournal duplicate(path);
        QVERIFY(!duplicate.save(state));
        const auto badPath = directory.filePath("failure.json");
        testing::RecoveryJournal failure(badPath);
        state["completed"] = false;
        QVERIFY(failure.save(state));
        QVERIFY(QDir().mkdir(badPath + ".progress/00000002.json"));
        QVERIFY(!failure.save(state));
        QVERIFY(failure.error().contains("already exists"));
        QVERIFY(!QFileInfo::exists(badPath));
        QVERIFY(QFileInfo::exists(badPath + ".progress/00000001.json"));
    }
    void closeActiveWorkerWithoutInvalidHandles() {
        const auto root = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (root.isEmpty())
            QSKIP("Set FLORA_TEST_CAPTURE_DIR for active original-worker shutdown");
        nativeCloseHandle = __imp_CloseHandle;
        DWORD oldProtection{};
        QVERIFY(VirtualProtect(&__imp_CloseHandle, sizeof __imp_CloseHandle, PAGE_READWRITE, &oldProtection));
        __imp_CloseHandle = observedCloseHandle;
        const auto restore = qScopeGuard([&] {
            __imp_CloseHandle = nativeCloseHandle;
            DWORD ignored{};
            VirtualProtect(&__imp_CloseHandle, sizeof __imp_CloseHandle, oldProtection, &ignored);
        });
        const auto probe = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        QVERIFY(probe);
        CloseHandle(probe);
        CloseHandle(probe);
        QCOMPARE(LONG(failedCloseCalls), LONG(1)); // Negative control proves the observer works.
        InterlockedExchange(&failedCloseCalls, 0);
        auto window = std::make_unique<MainWindow>();
        QSignalSpy done(window.get(), &MainWindow::taskFinished);
        window->openCapture(root + "/bf1_2026_01_21__16_53_05.gpa_frame");
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
        QVERIFY(done.last()[0].toBool());
        window->replay();
        HANDLE worker = nullptr;
        auto locate = [&] {
            const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof entry;
            if (Process32FirstW(snapshot, &entry))
                do {
                    if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                        QString::fromWCharArray(entry.szExeFile) == "FloraGPA.Worker.exe") {
                        worker = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                        if (worker) break;
                    }
                } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
            return worker != nullptr;
        };
        QTRY_VERIFY_WITH_TIMEOUT(locate(), 5000);
        const auto release = qScopeGuard([&] { CloseHandle(worker); });
        QTest::qWait(50); // Deliver QProcess::started and the job assignment.
        QVERIFY(window->busy());
        QCOMPARE(WaitForSingleObject(worker, 0), DWORD(WAIT_TIMEOUT));
        window.reset();
        QCOMPARE(WaitForSingleObject(worker, 5000), DWORD(WAIT_OBJECT_0));
        QCOMPARE(LONG(failedCloseCalls), LONG(0));
    }
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
        const auto trace = qEnvironmentVariable("FLORA_HEAP_TRACE_DIR");
        if (!trace.isEmpty()) testing::heapProbe::start();
        const auto stopTrace = qScopeGuard([&] { if (!trace.isEmpty()) testing::heapProbe::stop(); });
        if (!trace.isEmpty()) QVERIFY2(testing::heapProbe::selfCheck(), "UCRT allocation/free observer self-check");
        MainWindow window;
        QSignalSpy done(&window, &MainWindow::taskFinished);
        QSignalSpy loaded(&window, &MainWindow::captureLoaded);
        const QStringList files{"GF2_Exilium_2026_03_03__00_19_35.gpa_frame", "bf1_2026_01_21__16_53_05.gpa_frame"};
        const int iterations = qEnvironmentVariableIntValue("FLORA_RECOVERY_ITERATIONS");
        const int count = iterations > 0 ? iterations : 2;
        const int minimumSeconds = qEnvironmentVariableIntValue("FLORA_RECOVERY_MIN_SECONDS");
        QVERIFY(minimumSeconds >= 0 && minimumSeconds <= 86400);
        const auto journalPath = qEnvironmentVariable("FLORA_RECOVERY_JOURNAL");
        const auto workflows = qEnvironmentVariable("FLORA_RECOVERY_WORKFLOWS");
        const bool retentionControl = qEnvironmentVariableIsSet("FLORA_RECOVERY_RETENTION_CONTROL");
        QVERIFY(!retentionControl || (!journalPath.isEmpty() && qEnvironmentVariableIsSet("FLORA_RECOVERY_HEAP")));
        const auto memoryDirectory = qEnvironmentVariable("FLORA_MEMORY_MAP_DIR");
        QVERIFY(memoryDirectory.isEmpty() || (retentionControl && trace.isEmpty()));
        QVERIFY(journalPath.isEmpty() || !QFile::exists(journalPath));
        if (!journalPath.isEmpty()) {
            window.resize(1440, 900);
            window.show();
        }
        QElapsedTimer duration;
        duration.start();
        nlohmann::json journal{{"schema", "FloraGPA persistent Qt recovery soak 1"},
            {"qt_platform", QGuiApplication::platformName().toStdString()},
            {"started_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()},
            {"completed", false}, {"minimum_seconds", minimumSeconds},
            {"pixmap_cache_limit_kib", QPixmapCache::cacheLimit()},
            {"minimum_pairs", count}, {"observations", nlohmann::json::array()}};
        journal["workflows"] = !workflows.isEmpty();
        testing::RecoveryJournal journalWriter(journalPath);
        std::unique_ptr<testing::ProcessMemorySnapshot> memoryMaps;
        if (!memoryDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(memoryDirectory));
            memoryMaps = std::make_unique<testing::ProcessMemorySnapshot>();
        }
        auto memorySnapshot = [&](const char *name) {
            if (!memoryMaps) return true;
            const auto result = memoryMaps->save(QDir(memoryDirectory).filePath(QString::fromLatin1(name) + ".json"));
            journal["memory_maps"][name] = result;
            return result["address_walk_complete"].get<bool>() && result["heap_walk_complete"].get<bool>();
        };
        auto save = [&](const char *phase) {
            if (journalPath.isEmpty()) return true;
            journal["phase"] = phase;
            journal["elapsed_ms"] = duration.elapsed();
            return journalWriter.save(journal);
        };
        QTemporaryDir brokenDir;
        QVERIFY(brokenDir.isValid());
        const auto brokenPath = brokenDir.filePath("truncated.gpa_frame");
        QFile broken(brokenPath);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QCOMPARE(broken.write("IGPA"), qint64(4));
        broken.close();
        unsigned cycles = 0;
        QVERIFY(memorySnapshot("warmup"));
        QVERIFY2(save("starting"), qPrintable(journalWriter.error()));
        for (int repeat = 0; repeat < count || duration.elapsed() < qint64(minimumSeconds) * 1000; ++repeat) {
            for (const auto &name : files) {
                const unsigned epoch = unsigned(repeat * files.size() + files.indexOf(name) + 1);
                if (!trace.isEmpty()) testing::heapProbe::mark(epoch);
                QElapsedTimer elapsed;
                elapsed.start();
                const auto path = root + '/' + name;
                const QByteArray golden = name.startsWith("GF2")
                    ? "2e1abc5eacb0bbfd801f9fe059c305baa9a0786c384b36a42fa5a323dd979fd1"
                    : "1f724d1840652f66afd26dd30c95aecd5c1b4932eede56109198f15a8f57a2f6";
                QVERIFY(QFile::exists(path));
                const auto previous = window.capturePath();
                const auto before = loaded.size();
                journal["active_capture"] = name.toStdString();
                journal["active_pair"] = repeat;
                QVERIFY2(save("cancel_open"), qPrintable(journalWriter.error()));
                done.clear();
                window.openCapture(path);
                QVERIFY(window.busy());
                cancelAction(window)->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
                QVERIFY(!done.last()[0].toBool());
                QCOMPARE(window.capturePath(), previous);
                QCOMPARE(loaded.size(), before);
                QVERIFY2(save("open"), qPrintable(journalWriter.error()));
                done.clear();
                window.openCapture(path);
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
                QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                QCOMPARE(loaded.size(), before + 1);
                QVERIFY(!window.busy());
                QCOMPARE(window.capturePath(), path);
                QCOMPARE(outputHash(window), golden);
                nlohmann::json workflow;
                if (!workflows.isEmpty()) {
                    QVERIFY2(save("inspect_export"), qPrintable(journalWriter.error()));
                    bool passed = false;
                    const auto relative = QString("%1").arg(epoch, 6, 10, QChar('0'));
                    testing::recoveryWorkflows(window, QDir(workflows).filePath(relative), workflow, passed);
                    QVERIFY(passed);
                    workflow["directory"] = relative.toStdString();
                    QCOMPARE(outputHash(window), golden);
                }
                QVERIFY2(save("failed_open"), qPrintable(journalWriter.error()));
                done.clear();
                window.openCapture(brokenPath);
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
                QVERIFY(!done.last()[0].toBool());
                QCOMPARE(window.statusBar()->currentMessage(), QString("Capture header is truncated"));
                QCOMPARE(window.capturePath(), path);
                QCOMPARE(loaded.size(), before + 1);
                QCOMPARE(outputHash(window), golden);
                QVERIFY(!window.busy());
                QVERIFY2(save("cancel_replay"), qPrintable(journalWriter.error()));
                done.clear();
                window.replay();
                cancelAction(window)->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 10000);
                QVERIFY(!done.last()[0].toBool());
                QVERIFY(!window.busy());
                QVERIFY2(save("navigate"), qPrintable(journalWriter.error()));
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
                QVERIFY2(save("final_replay"), qPrintable(journalWriter.error()));
                boundary->setCurrentIndex(0);
                QTRY_VERIFY_WITH_TIMEOUT(!done.empty(), 60000);
                QVERIFY2(done.last()[0].toBool(), qPrintable(window.statusBar()->currentMessage()));
                QCOMPARE(outputHash(window), golden);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                PROCESS_MEMORY_COUNTERS_EX memory{};
                DWORD handles{};
                QVERIFY(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof memory));
                QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &handles));
                nlohmann::json observation{{"iteration", repeat}, {"capture", name.toStdString()},
                    {"elapsed_ms", elapsed.elapsed()}, {"private_bytes", memory.PrivateUsage},
                    {"working_set", memory.WorkingSetSize}, {"handles", handles},
                    {"gdi_objects", GetGuiResources(GetCurrentProcess(), 0)},
                    {"user_objects", GetGuiResources(GetCurrentProcess(), 1)},
                    {"window_visible", window.isVisible()},
                    {"window_exposed", window.windowHandle() && window.windowHandle()->isExposed()},
                    {"replay_status", window.statusBar()->currentMessage().toStdString()},
                    {"rgba_sha256", golden.toStdString()}, {"ownership", ownership(window)}};
                if (!workflows.isEmpty()) observation["workflows"] = std::move(workflow);
                qInfo().noquote() << "recovery_observation" << QString::fromStdString(observation.dump());
                ++cycles;
                if (cycles == 2) QVERIFY(memorySnapshot("baseline"));
                journal["observations"].push_back(observation);
                journal["completed_cycles"] = cycles;
                QVERIFY2(save("cycle_complete"), qPrintable(journalWriter.error()));
                if (!trace.isEmpty() && (epoch == 2 || epoch == 6 || epoch == unsigned(count * files.size())))
                    testing::heapProbe::snapshot(trace, epoch);
            }
        }
        if (retentionControl) {
            // Persist the original observations before isolating test-owned history.
            // Restore them after measurement; never discard evidence to pass a soak.
            const auto retainedPath = journalPath + ".retained.json";
            QVERIFY(!QFile::exists(retainedPath));
            {
                QSaveFile retained(retainedPath);
                QVERIFY(retained.open(QIODevice::WriteOnly));
                const auto bytes = journal.dump(2);
                QCOMPARE(retained.write(bytes.data(), qint64(bytes.size())), qint64(bytes.size()));
                QVERIFY2(retained.commit(), qPrintable(retained.errorString()));
            }
            const auto expectedPath = window.capturePath();
            const auto expectedImage = outputHash(window);
            auto sample = [&] {
                PROCESS_MEMORY_COUNTERS_EX memory{};
                if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof memory))
                    return nlohmann::json{{"sample_failed", true}};
                auto result = ownership(window);
                result["private_bytes"] = memory.PrivateUsage;
                result["working_set"] = memory.WorkingSetSize;
                return result;
            };
            nlohmann::json control{{"before", sample()}};
            QVERIFY(memorySnapshot("before_clear"));
            if (!trace.isEmpty()) {
                testing::heapProbe::snapshot(trace, cycles + 1);
                testing::heapProbe::mark(cycles + 1);
            }
            journal["observations"] = nlohmann::json::array();
            loaded.clear();
            done.clear();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            control["after_test_history_clear"] = sample();
            QVERIFY(memorySnapshot("after_history_clear"));
            if (!trace.isEmpty()) testing::heapProbe::snapshot(trace, cycles + 2);
            window.findChild<QPlainTextEdit *>("taskLog")->clear();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            control["after_log_clear"] = sample();
            QVERIFY(memorySnapshot("after_log_clear"));
            if (!trace.isEmpty()) testing::heapProbe::snapshot(trace, cycles + 3);
            QPixmapCache::clear();
            control["after_pixmap_cache_clear"] = sample();
            QVERIFY(memorySnapshot("after_pixmap_cache_clear"));
            if (!trace.isEmpty()) testing::heapProbe::snapshot(trace, cycles + 4);
            for (const auto &value : control)
                QVERIFY(value.value("heap_walk_complete", false));
            QCOMPARE(window.capturePath(), expectedPath);
            QCOMPARE(outputHash(window), expectedImage);
            {
                QFile retained(retainedPath);
                QVERIFY(retained.open(QIODevice::ReadOnly));
                auto restored = nlohmann::json::parse(retained.readAll().toStdString());
                QCOMPARE(restored.at("observations").size(), size_t(cycles));
                journal["observations"] = std::move(restored["observations"]);
            }
            journal["retention_control"] = control;
            qInfo().noquote() << "retention_control" << QString::fromStdString(control.dump());
        } else if (qEnvironmentVariableIsSet("FLORA_RECOVERY_HEAP")) {
            const auto before = ownership(window);
            window.findChild<QPlainTextEdit *>("taskLog")->clear();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            qInfo().noquote() << "log_clear_control" << QString::fromStdString(
                nlohmann::json({{"before", before}, {"after", ownership(window)}}).dump());
        }
        journal["completed"] = true;
        QVERIFY2(save("complete"), qPrintable(journalWriter.error()));
        qInfo() << "Original recovery cycles:" << cycles;
    }
};
QTEST_MAIN(RecoveryUiTests)
#include "RecoveryUiTests.moc"
