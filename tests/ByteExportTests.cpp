#include "app/ByteExport.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <Windows.h>
using namespace flora;
namespace {
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read test output");
    return f.readAll();
}
void save(const QString &path, const QByteArray &data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        throw std::runtime_error("Cannot write test fixture");
}
Bytes span(const QByteArray &data) {
    return {reinterpret_cast<const uint8_t *>(data.constData()), size_t(data.size())};
}
QByteArray pattern(int size) {
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) data[i] = char((i * 17u + (i >> 20)) & 255);
    return data;
}
}
class ByteExportTests final : public QObject {
    Q_OBJECT
  private slots:
    void complete_data() {
        QTest::addColumn<int>("size");
        for (int size : {0, 13, 1024 * 1024, 1024 * 1024 + 1, 3 * 1024 * 1024 + 17})
            QTest::newRow(qPrintable(QString::number(size))) << size;
    }
    void complete() {
        QFETCH(int, size);
        QTemporaryDir root;
        const auto path = root.filePath("saved.bin");
        const auto expected = pattern(size);
        exportBytesFile(path, span(expected));
        QCOMPARE(read(path), expected);
        save(path, "Previous data");
        exportBytesFile(path, span(expected));
        QCOMPARE(read(path), expected);
        QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"saved.bin"});
    }
    void cancelled_data() {
        QTest::addColumn<int>("stop");
        for (int stop = 1; stop <= 6; ++stop) QTest::newRow(qPrintable(QString::number(stop))) << stop;
    }
    void cancelled() {
        QFETCH(int, stop);
        QTemporaryDir root;
        const auto path = root.filePath("saved.bin");
        const auto data = pattern(3 * 1024 * 1024 + 17);
        for (bool existing : {false, true}) {
            if (existing) save(path, "Previous data");
            int calls = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                exportBytesFile(path, span(data), [&] { return ++calls == stop; }));
            QCOMPARE(calls, stop);
            QCOMPARE(QFileInfo::exists(path), existing);
            if (existing) QCOMPARE(read(path), QByteArray("Previous data"));
            QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden),
                     existing ? QStringList{"saved.bin"} : QStringList{});
        }
        exportBytesFile(path, span(data));
        QCOMPARE(read(path), data);
    }
    void lockedDestination() {
        QTemporaryDir root;
        const auto path = root.filePath("saved.bin");
        save(path, "Previous data");
        const auto lock = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        auto release = qScopeGuard([&] { CloseHandle(lock); });
        const auto expected = pattern(1024 * 1024 + 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportBytesFile(path, span(expected)));
        QCOMPARE(read(path), QByteArray("Previous data"));
        QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"saved.bin"});
        release.dismiss(); CloseHandle(lock);
        exportBytesFile(path, span(expected));
        QCOMPARE(read(path), expected);
    }
    void stagingFailure() {
        QTemporaryDir root;
        const auto path = root.filePath("directory");
        QVERIFY(QDir().mkpath(path));
        save(path + "/sentinel", "Existing directory data");
        const auto data = pattern(13);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportBytesFile(path, span(data)));
        QCOMPARE(read(path + "/sentinel"), QByteArray("Existing directory data"));
        exportBytesFile(root.filePath("retry.bin"), span(data));
        QCOMPARE(read(root.filePath("retry.bin")), data);
    }
};
QTEST_GUILESS_MAIN(ByteExportTests)
#include "ByteExportTests.moc"
