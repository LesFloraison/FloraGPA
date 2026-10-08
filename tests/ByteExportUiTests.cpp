#include "MsaaCapture.h"
#include "app/MainWindow.h"
#include "app/Appearance.h"
#include <QAction>
#include <QFileDialog>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QtTest>
using namespace flora;
namespace {
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read export fixture");
    return file.readAll();
}
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write export fixture");
}
QAction *exportAction(MainWindow &window, const QString &kind) {
    if (kind == "storage") return window.findChild<QAction *>("exportOutputStorage");
    if (kind == "resource") {
        for (auto action : window.findChildren<QAction *>())
            if (action->text() == "Export Resource…") return action;
    } else {
        auto read = window.findChild<QAction *>("readBuffer");
        for (auto action : read->parent()->findChildren<QAction *>())
            if (action->text() == "Export") return action;
    }
    return nullptr;
}
void choose(MainWindow &window, QAction *action, const QString &path) {
    bool seen = false;
    QTimer chooser, watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&chooser, &QTimer::timeout, &chooser, [&] {
        auto dialog = window.findChild<QFileDialog *>();
        if (!dialog) return;
        chooser.stop(); seen = true;
        dialog->setOption(QFileDialog::DontConfirmOverwrite);
        auto filename = dialog->findChild<QLineEdit *>("fileNameEdit");
        QVERIFY(filename);
        filename->setText(path);
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
    });
    QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&] {
        if (auto dialog = window.findChild<QFileDialog *>()) dialog->reject();
    });
    chooser.start(10); watchdog.start(30000);
    action->trigger();
    QVERIFY(seen);
}
bool selectResource(MainWindow &window, Id id) {
    const auto view = window.findChild<QTableView *>("resources");
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        auto index = view->model()->index(row, 0);
        if (index.data(Qt::UserRole).toULongLong() == id) { view->setCurrentIndex(index); return true; }
    }
    return false;
}
}
class ByteExportUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA-ByteExportTests");
        QCoreApplication::setApplicationName("FloraGPA-ByteExportTests");
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        applyAppearance(*qApp);
    }
    void lifecycle_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("mode");
        for (const auto kind : {"buffer", "resource"})
            for (const auto mode : {"success", "cancel", "switch", "close", "destroy", "locked"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(kind, mode))) << QString(kind) << QString(mode);
    }
    void lifecycle() {
        QFETCH(QString, kind); QFETCH(QString, mode);
        QTemporaryDir root;
        const auto capturePath = root.filePath("large.gpa_frame"), replacement = root.filePath("second.gpa_frame");
        const QByteArray expected(64 * 1024 * 1024, 'C');
        {
            auto capture = testing::msaaOutputCapture(false);
            std::vector<uint8_t> resource(16);
            testing::append(resource, D3D11_BUFFER_DESC{UINT(expected.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0});
            testing::append(resource, Id(81)); capture.add(80, 5, 0x83, resource);
            auto initial = testing::word(UINT(expected.size()));
            initial.insert(initial.end(), expected.constData(), expected.constData() + expected.size());
            capture.add(81, 9, 1, std::move(initial)); capture.save(capturePath);
            testing::msaaOutputCapture(false).save(replacement);
        }
        auto owner = std::make_unique<MainWindow>(); auto &window = *owner;
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(capturePath);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000);
        QVERIFY(done.last()[0].toBool());
        // Raw resource export uses the mapped full resource independently of
        // the buffer inspector's selected byte range.
        if (kind == "resource") window.findChild<QLineEdit *>("bufferLength")->setText("16");
        done.clear(); QVERIFY(selectResource(window, 80));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000);
        QVERIFY(done.last()[0].toBool());
        if (kind == "buffer") {
            auto model = static_cast<BufferModel *>(window.findChild<QTableView *>("bufferTable")->model());
            QCOMPARE(model->bytes(), expected);
        }
        auto action = exportAction(window, kind); QVERIFY(action && action->isEnabled());
        const auto destination = root.filePath("saved.bin");
        save(destination, "Previous data");
        HANDLE lock = INVALID_HANDLE_VALUE;
        if (mode == "locked") {
            lock = CreateFileW(reinterpret_cast<const wchar_t *>(destination.utf16()), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(lock != INVALID_HANDLE_VALUE);
        }
        auto release = qScopeGuard([&] { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); });
        bool seen = false, heartbeat = false;
        int ticks = 0;
        QTimer pulse;
        connect(&pulse, &QTimer::timeout, &pulse, [&] { if (owner && owner->busy()) ++ticks; });
        const auto observer = connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &message) {
            if (seen || message != QString("Exporting %1…").arg(kind)) return;
            seen = true; QVERIFY(window.busy()); pulse.start(1);
            QTimer::singleShot(0, &window, [&] {
                heartbeat = true;
                if (mode == "success" || mode == "locked") return;
                if (mode == "destroy") owner.reset();
                else if (mode == "close") window.close();
                else if (mode == "switch") { done.clear(); window.openCapture(replacement); }
                else {
                    QAction *cancel = nullptr;
                    for (auto candidate : window.findChildren<QAction *>()) if (candidate->text() == "Cancel") cancel = candidate;
                    QVERIFY(cancel && cancel->isEnabled()); cancel->trigger();
                }
            });
        });
        choose(window, action, destination);
        if (mode == "destroy") {
            QTRY_VERIFY_WITH_TIMEOUT(!owner, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(capturePath) || QFile::remove(capturePath), 30000); // Span owner released.
            QVERIFY(seen && heartbeat); QCOMPARE(exported.size(), 0);
            QCOMPARE(read(destination), QByteArray("Previous data")); return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        disconnect(observer); pulse.stop();
        QVERIFY(seen && heartbeat);
        QCOMPARE(exported.last()[0].toBool(), mode == "success");
        QCOMPARE(read(destination), mode == "success" ? expected : QByteArray("Previous data"));
        if (mode == "success") { QVERIFY(ticks > 0); qInfo() << "Byte export UI ticks:" << ticks; }
        if (lock != INVALID_HANDLE_VALUE) { CloseHandle(lock); lock = INVALID_HANDLE_VALUE; }
        window.show(); done.clear(); window.openCapture(capturePath);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        done.clear(); QVERIFY(selectResource(window, 80));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        // Retry the same export action after failure, cancellation or switching.
        exported.clear(); choose(window, exportAction(window, kind), destination);
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
        QCOMPARE(read(destination), expected);
    }
    void textureAction_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("resource") << QString("resource");
        QTest::newRow("storage") << QString("storage");
    }
    void textureAction() {
        QFETCH(QString, kind);
        QTemporaryDir root; const auto path = root.filePath("frame.gpa_frame");
        testing::msaaOutputCapture(false).save(path);
        MainWindow window; window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(path);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        done.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(1, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(2);
        QVERIFY(selectResource(window, 20));
        auto texture = window.findChild<QAction *>("exportTexture");
        QTRY_VERIFY_WITH_TIMEOUT(texture->isEnabled() && !window.busy(), 30000);
        choose(window, exportAction(window, kind), root.filePath("saved.bin"));
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
        QVERIFY(texture->isEnabled());
    }
};
QTEST_MAIN(ByteExportUiTests)
#include "ByteExportUiTests.moc"
