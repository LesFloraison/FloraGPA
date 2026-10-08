#include "MsaaCapture.h"
#include "app/MainWindow.h"
#include "app/Appearance.h"
#include "application/ApiCommands.h"
#include <QCryptographicHash>
#include <QAbstractButton>
#include <QFileDialog>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QThreadPool>
#include <QtTest>
using namespace flora;
namespace {
QByteArray read(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Fixture read failed"); return f.readAll(); }
void save(const QString &path, const QByteArray &bytes) { QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) throw std::runtime_error("Fixture write failed"); }
QByteArray digest(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Fixture hash failed"); QCryptographicHash hash(QCryptographicHash::Sha256); if (!hash.addData(&f)) throw std::runtime_error("Fixture hash failed"); return hash.result(); }
void filters(MainWindow &window, bool gpu = false, const QString &text = {}, const QString &resource = {}) {
    window.findChild<QComboBox *>("apiKinds")->setCurrentIndex(gpu ? 0 : 1);
    window.findChild<QLineEdit *>("apiSearch")->setText(text);
    window.findChild<QLineEdit *>("apiResourceFilter")->setText(resource);
}
void choose(MainWindow &window, const QString &path) {
    bool seen = false;
    QTimer chooser, watchdog; watchdog.setSingleShot(true);
    QObject::connect(&chooser, &QTimer::timeout, &chooser, [&] {
        if (auto box = window.findChild<QMessageBox *>()) { box->button(QMessageBox::Yes)->click(); return; }
        if (auto dialog = window.findChild<QFileDialog *>()) {
            seen = true;
            auto edit = dialog->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(edit); edit->setText(path);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        }
    });
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject();
        if (auto box = window.findChild<QMessageBox *>()) box->button(QMessageBox::No)->click();
    });
    chooser.start(10); watchdog.start(30000);
    window.findChild<QAction *>("exportApiLog")->trigger(); QVERIFY(seen);
}
}
class ApiExportUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA-ApiExportTests");
        QCoreApplication::setApplicationName("FloraGPA-ApiExportTests");
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        applyAppearance(*qApp);
    }
    void ownership_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"capture", "filter", "confirmation-capture", "confirmation-filter"}) QTest::newRow(mode) << QString(mode);
    }
    void ownership() {
        QFETCH(QString, mode);
        const auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty()) QSKIP("Set FLORA_TEST_CAPTURE_DIR for original API export ownership");
        const auto path = captures + "/GF2_Exilium_2026_03_03__00_19_35.gpa_frame";
        QTemporaryDir root; const auto replacement = root.filePath("replacement.gpa_frame"); testing::msaaOutputCapture(false).save(replacement);
        Frame frame(path.toStdWString());
        exportCommands(frame, root.filePath("expected").toStdWString(), "Map", Id(6), false);
        QVERIFY(!nlohmann::json::parse(read(root.filePath("expected/commands.json")).toStdString())["commands"].empty());
        MainWindow window; window.show(); QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(path); QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        filters(window, false, "Map", "6");
        const auto target = root.filePath("actual"); QVERIFY(QDir().mkpath(target));
        const bool confirm = mode.startsWith("confirmation-");
        if (confirm) { save(target + "/commands.json", "Old JSON"); save(target + "/commands.csv", "Old CSV"); }
        bool seen = false, changed = false, confirmed = false;
        QTimer poll, watchdog; watchdog.setSingleShot(true);
        const auto change = [&] {
            if (!changed) {
                changed = true;
                if (mode.endsWith("capture")) { done.clear(); window.openCapture(replacement); }
                else filters(window, true, "NoSuchCommand", "0");
            }
            return !mode.endsWith("capture") || (!done.empty() && !window.busy());
        };
        connect(&poll, &QTimer::timeout, &poll, [&] {
            if (auto box = window.findChild<QMessageBox *>()) {
                if (!change()) return;
                confirmed = true; box->button(QMessageBox::Yes)->click(); return;
            }
            if (auto dialog = window.findChild<QFileDialog *>()) {
                seen = true; if (!confirm && !change()) return;
                auto edit = dialog->findChild<QLineEdit *>("fileNameEdit"); QVERIFY(edit); edit->setText(target);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
            if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject();
            if (auto box = window.findChild<QMessageBox *>()) box->button(QMessageBox::No)->click();
        });
        poll.start(10); watchdog.start(30000); window.findChild<QAction *>("exportApiLog")->trigger(); poll.stop(); watchdog.stop();
        QVERIFY(seen && changed); QCOMPARE(confirmed, confirm);
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
        for (const auto name : {"commands.json", "commands.csv"}) QCOMPARE(read(target + '/' + name), read(root.filePath("expected/") + name));
    }
    void lifecycle_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"success", "cancel", "switch", "close", "destroy", "json-lock", "csv-lock"}) QTest::newRow(mode) << QString(mode);
    }
    void lifecycle() {
        QFETCH(QString, mode);
        const auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty()) QSKIP("Set FLORA_TEST_CAPTURE_DIR for original API export lifecycle");
        const auto path = captures + "/bf1_2026_01_21__16_53_05.gpa_frame";
        QTemporaryDir root; const auto replacement = root.filePath("replacement.gpa_frame"); testing::msaaOutputCapture(false).save(replacement);
        Frame frame(path.toStdWString()); exportCommands(frame, root.filePath("expected").toStdWString());
        const auto target = root.filePath("actual"); QVERIFY(QDir().mkpath(target));
        save(target + "/commands.json", "Old JSON"); save(target + "/commands.csv", "Old CSV");
        auto owner = std::make_unique<MainWindow>(); auto &window = *owner; window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(path); QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool()); filters(window);
        HANDLE lock = INVALID_HANDLE_VALUE; auto release = qScopeGuard([&] { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); });
        if (mode.endsWith("-lock")) {
            const auto file = target + (mode == "json-lock" ? "/commands.json" : "/commands.csv");
            lock = CreateFileW(reinterpret_cast<const wchar_t *>(file.utf16()), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr); QVERIFY(lock != INVALID_HANDLE_VALUE);
        }
        bool seen = false, heartbeat = false; int ticks = 0; QTimer pulse;
        connect(&pulse, &QTimer::timeout, &pulse, [&] { if (owner && owner->busy()) ++ticks; });
        const auto observer = connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &message) {
            if (seen || message != "Exporting API log…") return;
            seen = true; QVERIFY(window.busy()); pulse.start(1);
            QTimer::singleShot(0, &window, [&] {
                heartbeat = true;
                if (mode == "destroy") owner.reset();
                else if (mode == "close") window.close();
                else if (mode == "switch") { done.clear(); window.openCapture(replacement); }
                else if (mode == "cancel") {
                    QAction *cancel = nullptr; for (auto action : window.findChildren<QAction *>()) if (action->text() == "Cancel") cancel = action;
                    QVERIFY(cancel && cancel->isEnabled()); cancel->trigger();
                }
            });
        });
        choose(window, target);
        if (mode == "destroy") {
            QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
            QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(), 0, 30000);
            QVERIFY(seen && heartbeat); QCOMPARE(exported.size(), 0);
            QCOMPARE(read(target + "/commands.json"), QByteArray("Old JSON")); QCOMPARE(read(target + "/commands.csv"), QByteArray("Old CSV")); return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        disconnect(observer); pulse.stop(); QVERIFY(seen && heartbeat); QCOMPARE(exported.last()[0].toBool(), mode == "success");
        if (mode == "success" || mode == "csv-lock") QCOMPARE(digest(target + "/commands.json"), digest(root.filePath("expected/commands.json")));
        else QCOMPARE(read(target + "/commands.json"), QByteArray("Old JSON"));
        if (mode == "success") { QCOMPARE(digest(target + "/commands.csv"), digest(root.filePath("expected/commands.csv"))); QVERIFY(ticks > 0); qInfo() << "API export UI ticks:" << ticks; }
        else QCOMPARE(read(target + "/commands.csv"), QByteArray("Old CSV"));
        if (mode == "csv-lock") QVERIFY(window.findChild<QPlainTextEdit *>("taskLog")->toPlainText().contains("partially published"));
        if (lock != INVALID_HANDLE_VALUE) { CloseHandle(lock); lock = INVALID_HANDLE_VALUE; }
        window.show(); done.clear(); window.openCapture(path); QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool()); filters(window);
        exported.clear(); choose(window, target); QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
        for (const auto name : {"commands.json", "commands.csv"}) QCOMPARE(digest(target + '/' + name), digest(root.filePath("expected/") + name));
    }
};
QTEST_MAIN(ApiExportUiTests)
#include "ApiExportUiTests.moc"
