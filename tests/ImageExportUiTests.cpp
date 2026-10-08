#include "MsaaCapture.h"
#include "app/MainWindow.h"
#include "app/Appearance.h"
#include <QAction>
#include <QFileDialog>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStatusBar>
#include <QThreadPool>
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
    if (kind == "texture") return window.findChild<QAction *>("exportTexture");
    for (auto action : window.findChildren<QAction *>())
        if (action->shortcut() == QKeySequence("Ctrl+Shift+S")) return action;
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
class ImageExportUiTests final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("FloraGPA-ImageExportTests");
        QCoreApplication::setApplicationName("FloraGPA-ImageExportTests");
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        applyAppearance(*qApp);
    }
    void refreshDuringImageExport() {
        QTemporaryDir root;
        const auto capture = root.filePath("msaa.gpa_frame");
        testing::msaaOutputCapture(false).save(capture);
        MainWindow window;
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(capture);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000);
        QVERIFY(done.last()[0].toBool());
        done.clear();
        auto api = window.findChild<QTableView *>("apiLog");
        api->setCurrentIndex(api->model()->index(1, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000);
        QVERIFY(done.last()[0].toBool());
        window.findChild<QComboBox *>("textureBoundary")->setCurrentIndex(2);
        QVERIFY(selectResource(window, 20));
        auto textureAction = exportAction(window, "texture");
        QTRY_VERIFY_WITH_TIMEOUT(textureAction->isEnabled() && !window.busy(), 30000);
        auto viewer = window.findChild<ImageView *>("textureOutput");
        QCOMPARE(viewer->image().pixelColor(0, 0), QColor(10, 0, 0, 255));
        // Keep encoding busy while a real small texture preview is accepted.
        // This display-only fixture does not change capture/replay semantics.
        QImage image(4096, 4096, QImage::Format_RGB32);
        quint32 random = 19;
        for (int y = 0; y < image.height(); ++y) {
            auto row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                row[x] = random | 0xff000000;
            }
        }
        viewer->setImage(image);
        const auto snapshot = viewer->displayImage();
        bool launched = false, overlapped = false;
        connect(window.statusBar(), &QStatusBar::messageChanged, &window, [&](const QString &message) {
            if (launched || message != "Exporting image…") return;
            launched = true;
            QTimer::singleShot(0, &window, [&] { window.findChild<QSpinBox *>("textureSample")->setValue(3); });
        });
        connect(&window, &MainWindow::taskFinished, &window, [&](bool success) {
            if (success && !viewer->image().isNull() && viewer->image().pixelColor(0, 0) == QColor(16, 0, 0, 255))
                overlapped = exported.empty() && window.busy() && !textureAction->isEnabled();
        });
        const auto path = root.filePath("snapshot.png");
        choose(window, exportAction(window, "image"), path);
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        QVERIFY(launched && overlapped);
        QVERIFY(exported.last()[0].toBool());
        QVERIFY(textureAction->isEnabled());
        QCOMPARE(viewer->image().pixelColor(0, 0), QColor(16, 0, 0, 255));
        QCOMPARE(QImage(path).convertToFormat(QImage::Format_RGBA8888), snapshot.convertToFormat(QImage::Format_RGBA8888));
    }
    void lifecycle_data() {
        QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("mode");
        for (const auto kind : {"image", "texture"})
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
            if (kind == "texture") {
                capture.add(80, 5, 0x85, testing::statePack(Id(0), Id(0), 4096u, 4096u,
                    1u, 1u, 28u, 1u, 0u, 0u, 8u, 0u, 0u, Id(81)));
                auto initial = testing::word(UINT(expected.size()));
                initial.insert(initial.end(), expected.constData(), expected.constData() + expected.size());
                capture.add(81, 9, 1, std::move(initial));
            }
            capture.save(capturePath);
            testing::msaaOutputCapture(false).save(replacement);
        }
        auto owner = std::make_unique<MainWindow>(); auto &window = *owner;
        window.show();
        QSignalSpy done(&window, &MainWindow::taskFinished), exported(&window, &MainWindow::exportFinished);
        window.openCapture(capturePath);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000);
        QVERIFY(done.last()[0].toBool());
        QImage displayed;
        const auto prepare = [&] {
            if (kind == "texture") {
                done.clear(); QVERIFY(selectResource(window, 80));
                QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QAction *>("exportTexture")->isEnabled() && !window.busy(), 30000);
            } else {
                QImage image(2048, 2048, QImage::Format_RGB32);
                quint32 random = 19;
                for (int y = 0; y < image.height(); ++y) {
                    auto row = reinterpret_cast<QRgb *>(image.scanLine(y));
                    for (int x = 0; x < image.width(); ++x) {
                        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                        row[x] = random | 0xff000000;
                    }
                }
                auto viewer = window.findChild<ImageView *>("frameOutput");
                viewer->setImage(image); displayed = viewer->displayImage();
            }
        };
        prepare();
        auto action = exportAction(window, kind); QVERIFY(action && action->isEnabled());
        const auto destination = root.filePath(kind == "image" ? "saved.png" : "saved.bin");
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
            QTRY_COMPARE_WITH_TIMEOUT(QThreadPool::globalInstance()->activeThreadCount(), 0, 30000);
            QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(capturePath) || QFile::remove(capturePath), 30000);
            QVERIFY(seen && heartbeat); QCOMPARE(exported.size(), 0);
            QCOMPARE(read(destination), QByteArray("Previous data")); return;
        }
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.busy(), 30000);
        disconnect(observer); pulse.stop();
        QVERIFY(seen && heartbeat);
        QCOMPARE(exported.last()[0].toBool(), mode == "success");
        if (mode != "success") QCOMPARE(read(destination), QByteArray("Previous data"));
        else if (kind == "texture") QCOMPARE(read(destination), expected);
        else QCOMPARE(QImage(destination).convertToFormat(QImage::Format_RGBA8888), displayed.convertToFormat(QImage::Format_RGBA8888));
        if (mode == "success") { QVERIFY(ticks > 0); qInfo() << "Image/texture export UI ticks:" << ticks; }
        if (lock != INVALID_HANDLE_VALUE) { CloseHandle(lock); lock = INVALID_HANDLE_VALUE; }
        window.show(); done.clear(); window.openCapture(capturePath);
        QTRY_VERIFY_WITH_TIMEOUT(!done.empty() && !window.busy(), 30000); QVERIFY(done.last()[0].toBool());
        prepare();
        // Retry the same export action after failure, cancellation or switching.
        exported.clear(); choose(window, exportAction(window, kind), destination);
        QTRY_COMPARE_WITH_TIMEOUT(exported.size(), 1, 30000); QVERIFY(exported.last()[0].toBool());
        if (kind == "texture") QCOMPARE(read(destination), expected);
        else QCOMPARE(QImage(destination).convertToFormat(QImage::Format_RGBA8888), displayed.convertToFormat(QImage::Format_RGBA8888));
    }
 };
QTEST_MAIN(ImageExportUiTests)
#include "ImageExportUiTests.moc"
