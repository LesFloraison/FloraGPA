#include "app/ImageExport.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <Windows.h>
using namespace flora;
namespace {
QByteArray read(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read image fixture");
    return f.readAll();
}
void save(const QString &path, const QByteArray &bytes) {
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot save image fixture");
}
QImage pixels() {
    QImage image(513, 257, QImage::Format_RGBA8888);
    quint32 state = 17;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.bytesPerLine(); ++x) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        image.scanLine(y)[x] = uchar(state);
    }
    image.setText("Description", "Export fixture");
    return image;
}
}
class ImageExportTests final : public QObject {
    Q_OBJECT
  private slots:
    void formats_data() {
        QTest::addColumn<QString>("suffix");
        for (auto suffix : {"png", "PNG", "bmp", "jpg", "ico", "cur", "pbm", "pgm", "ppm", "xbm", "xpm"})
            QTest::newRow(suffix) << QString(suffix);
    }
    void formats() {
        QFETCH(QString, suffix);
        QTemporaryDir root; const auto image = pixels();
        const auto oldPath = root.filePath("old." + suffix), path = root.filePath("new." + suffix);
        QVERIFY(image.save(oldPath)); // Previous QImage::save route.
        save(path, "Previous data"); exportImageFile(path, image);
        QCOMPARE(read(path), read(oldPath));
        QCOMPARE(QImage(path), QImage(oldPath));
    }
    void imageCancellation_data() {
        QTest::addColumn<int>("stop");
        for (int stop : {1, 2, 5, 12}) QTest::newRow(qPrintable(QString::number(stop))) << stop;
    }
    void imageCancellation() {
        QFETCH(int, stop);
        QTemporaryDir root; const auto path = root.filePath("saved.png");
        save(path, "Previous data"); int calls = 0;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, exportImageFile(path, pixels(), [&] { return ++calls == stop; }));
        QCOMPARE(calls, stop); QCOMPARE(read(path), QByteArray("Previous data"));
        QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"saved.png"});
        exportImageFile(path, pixels()); QVERIFY(!QImage(path).isNull());
    }
    void callbackException() {
        QTemporaryDir root; const auto path = root.filePath("saved.png");
        save(path, "Previous data"); int calls = 0;
        QVERIFY_THROWS_EXCEPTION(std::logic_error, exportImageFile(path, pixels(), [&]() -> bool {
            if (++calls == 2) throw std::logic_error("Injected callback failure"); return false;
        }));
        QCOMPARE(read(path), QByteArray("Previous data"));
        exportImageFile(path, pixels()); QVERIFY(!QImage(path).isNull());
    }
    void invalidImages() {
        QTemporaryDir root;
        for (const auto suffix : {"png", "unsupported"}) {
            const auto path = root.filePath(QString("saved.") + suffix); save(path, "Previous data");
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportImageFile(path, QString(suffix) == "png" ? QImage() : pixels()));
            QCOMPARE(read(path), QByteArray("Previous data"));
        }
    }
    void copy_data() {
        QTest::addColumn<int>("size");
        for (int size : {0, 13, 1024 * 1024, 3 * 1024 * 1024 + 17}) QTest::newRow(qPrintable(QString::number(size))) << size;
    }
    void copy() {
        QFETCH(int, size);
        QTemporaryDir root; const auto source = root.filePath("source"), path = root.filePath("saved");
        QByteArray bytes(size, Qt::Uninitialized);
        for (int i = 0; i < size; ++i) bytes[i] = char(i * 31u + (i >> 20));
        save(source, bytes); save(path, "Previous data"); copyExportFile(source, path);
        QCOMPARE(read(path), bytes);
    }
    void copyInterrupted_data() {
        QTest::addColumn<QString>("mode");
        for (auto mode : {"before", "middle", "before-commit", "shrink", "grow", "missing"})
            QTest::newRow(mode) << QString(mode);
    }
    void copyInterrupted() {
        QFETCH(QString, mode);
        QTemporaryDir root; const auto source = root.filePath("source"), path = root.filePath("saved");
        const QByteArray bytes(3 * 1024 * 1024 + 17, 'x'); save(source, bytes); save(path, "Previous data");
        int calls = 0;
        const auto cancelled = [&] {
            ++calls;
            if (mode == "before") return calls == 1;
            if (mode == "middle") return calls == 3;
            if (mode == "before-commit") return calls == 6;
            if (calls == 3 && (mode == "shrink" || mode == "grow")) {
                QFile file(source); if (!file.open(QIODevice::ReadWrite) || !file.resize(mode == "shrink" ? 0 : bytes.size() + 1))
                    throw std::logic_error("Cannot alter copy fixture");
            }
            return false;
        };
        if (mode == "missing") QVERIFY(QFile::remove(source));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, copyExportFile(source, path, cancelled));
        QCOMPARE(read(path), QByteArray("Previous data"));
        save(source, bytes); copyExportFile(source, path); QCOMPARE(read(path), bytes);
    }
    void publication_data() {
        QTest::addColumn<bool>("image"); QTest::newRow("image") << true; QTest::newRow("copy") << false;
    }
    void publication() {
        QFETCH(bool, image);
        QTemporaryDir root; const auto source = root.filePath("source"), path = root.filePath("saved.png");
        save(source, "Texture bytes"); save(path, "Previous data");
        const auto lock = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE); auto release = qScopeGuard([&] { CloseHandle(lock); });
        const auto run = [&] { if (image) exportImageFile(path, pixels()); else copyExportFile(source, path); };
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, run()); QCOMPARE(read(path), QByteArray("Previous data"));
        QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden), (QStringList{"saved.png", "source"}));
        release.dismiss(); CloseHandle(lock); run();
        if (image) QVERIFY(!QImage(path).isNull()); else QCOMPARE(read(path), QByteArray("Texture bytes"));
    }
};
QTEST_GUILESS_MAIN(ImageExportTests)
#include "ImageExportTests.moc"
