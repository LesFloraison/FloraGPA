#include "app/OutputStorageExport.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
#include <Windows.h>
using namespace flora;
using Json = nlohmann::json;
namespace {
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Test fixture write failed");
}
QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Test fixture read failed");
    return file.readAll();
}
struct Fixture {
    QTemporaryDir root;
    QByteArray data = QByteArray(3 * 1024 * 1024 + 17, 'x');
    Json display;
    QString target() const { return root.filePath("saved.bin"); }
    QString raw() const { return root.filePath("output_storage.bin"); }
    QString metadata() const { return root.filePath("output_storage.json"); }
    void initialize() {
        display = {{"resource", 20}, {"storage_bytes", data.size()},
                   {"storage_sha256", QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().toStdString()}};
        save(raw(), data);
        save(metadata(), QByteArray::fromStdString(display.dump(2) + "\n"));
        save(target(), "Previous binary"); save(target() + ".json", "Previous metadata");
    }
};
}
class OutputStorageExportTests final : public QObject {
    Q_OBJECT
  private slots:
    void valid_data() {
        QTest::addColumn<int>("size");
        QTest::newRow("empty") << 0;
        QTest::newRow("small") << 13;
        QTest::newRow("several-chunks") << 3 * 1024 * 1024 + 17;
    }
    void valid() {
        QFETCH(int, size);
        Fixture f; f.data.resize(size); f.initialize();
        const auto listing = QDir(f.root.path()).entryList(QDir::Files | QDir::Hidden);
        exportOutputStorageFiles(f.root.path(), f.target(), f.display);
        QCOMPARE(read(f.target()), f.data);
        QCOMPARE(read(f.target() + ".json"), read(f.metadata()));
        QCOMPARE(QDir(f.root.path()).entryList(QDir::Files | QDir::Hidden), listing);
    }
    void rejects_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"missing", "short", "long", "hash", "metadata-missing", "metadata-short",
                               "metadata-identity", "count-missing", "count-negative", "count-float",
                               "count-string", "count-overflow", "hash-missing", "hash-invalid", "hash-type",
                               "alias-raw", "alias-metadata", "metadata-directory",
                               "truncate-during-read", "grow-during-read", "change-during-read"})
            QTest::newRow(mode) << QString(mode);
    }
    void rejects() {
        QFETCH(QString, mode);
        Fixture f; f.initialize();
        auto target = f.target();
        if (mode == "missing") QVERIFY(QFile::remove(f.raw()));
        if (mode == "short") save(f.raw(), f.data.left(f.data.size() - 1));
        if (mode == "long") save(f.raw(), f.data + "x");
        if (mode == "hash") { auto data = f.data; data[0] = 'z'; save(f.raw(), data); }
        if (mode == "metadata-missing") QVERIFY(QFile::remove(f.metadata()));
        if (mode == "metadata-short") save(f.metadata(), read(f.metadata()).chopped(1));
        if (mode == "metadata-identity") {
            auto metadata = f.display; metadata["resource"] = 21;
            save(f.metadata(), QByteArray::fromStdString(metadata.dump(2) + "\n"));
        }
        if (mode == "count-missing") f.display.erase("storage_bytes");
        if (mode == "count-negative") f.display["storage_bytes"] = -1;
        if (mode == "count-float") f.display["storage_bytes"] = 1.5;
        if (mode == "count-string") f.display["storage_bytes"] = "3";
        if (mode == "count-overflow") f.display["storage_bytes"] = UINT64_MAX;
        if (mode == "hash-missing") f.display.erase("storage_sha256");
        if (mode == "hash-invalid") f.display["storage_sha256"] = "incorrect";
        if (mode == "hash-type") f.display["storage_sha256"] = 42;
        if (mode == "alias-raw") target = f.raw();
        if (mode == "alias-metadata") target = f.metadata();
        if (mode == "metadata-directory") {
            QVERIFY(QFile::remove(f.target() + ".json"));
            QVERIFY(QDir().mkpath(f.target() + ".json"));
        }
        const auto listing = QDir(f.root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        int checks = 0; bool mutated = false;
        auto cancelled = [&] {
            if (++checks == 4 && mode.endsWith("during-read")) {
                QFile file(f.raw());
                if (!file.open(QIODevice::ReadWrite)) throw std::runtime_error("Cannot mutate test input");
                if (mode == "truncate-during-read") mutated = file.resize(1);
                if (mode == "grow-during-read") mutated = file.resize(f.data.size() + 1);
                if (mode == "change-during-read") mutated = file.seek(2 * 1024 * 1024) && file.write("z", 1) == 1;
                if (!file.flush()) throw std::runtime_error("Cannot flush test input");
            }
            return false;
        };
        QVERIFY_THROWS_EXCEPTION(std::exception, exportOutputStorageFiles(f.root.path(), target, f.display, cancelled));
        if (mode.endsWith("during-read")) QVERIFY(mutated);
        QCOMPARE(read(f.target()), QByteArray("Previous binary"));
        if (mode != "metadata-directory") QCOMPARE(read(f.target() + ".json"), QByteArray("Previous metadata"));
        QCOMPARE(QDir(f.root.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), listing);
        // A failed export can be retried using the same requested destination.
        if (mode == "metadata-directory") QVERIFY(QDir().rmdir(f.target() + ".json"));
        f.initialize();
        exportOutputStorageFiles(f.root.path(), f.target(), f.display);
        QCOMPARE(read(f.target()), f.data);
    }
    void cancellation() {
        for (int stop = 1; stop <= 7; ++stop) {
            Fixture f; f.initialize();
            int checks = 0;
            const auto listing = QDir(f.root.path()).entryList(QDir::Files | QDir::Hidden);
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                exportOutputStorageFiles(f.root.path(), f.target(), f.display, [&] { return ++checks == stop; }));
            QCOMPARE(read(f.target()), QByteArray("Previous binary"));
            QCOMPARE(read(f.target() + ".json"), QByteArray("Previous metadata"));
            QCOMPARE(QDir(f.root.path()).entryList(QDir::Files | QDir::Hidden), listing);
        }
    }
    void publicationFailure_data() {
        QTest::addColumn<bool>("metadata");
        QTest::newRow("binary-locked") << false;
        QTest::newRow("metadata-locked") << true;
    }
    void publicationFailure() {
        QFETCH(bool, metadata);
        Fixture f; f.initialize();
        const auto path = f.target() + (metadata ? ".json" : "");
        const auto lock = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        auto release = qScopeGuard([&] { CloseHandle(lock); });
        try {
            exportOutputStorageFiles(f.root.path(), f.target(), f.display);
            QFAIL("Locked destination publication succeeded");
        } catch (const std::exception &error) {
            QVERIFY2(QString::fromUtf8(error.what()).contains(metadata ? "metadata publication failed" : "Cannot publish"), error.what());
        }
        QCOMPARE(read(f.target()), metadata ? f.data : QByteArray("Previous binary"));
        QCOMPARE(read(f.target() + ".json"), QByteArray("Previous metadata"));
        release.dismiss(); CloseHandle(lock);
        exportOutputStorageFiles(f.root.path(), f.target(), f.display);
        QCOMPARE(read(f.target()), f.data);
        QCOMPARE(read(f.target() + ".json"), read(f.metadata()));
    }
};
QTEST_GUILESS_MAIN(OutputStorageExportTests)
#include "OutputStorageExportTests.moc"
