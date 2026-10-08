#include "app/GeometryExport.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <Windows.h>
using namespace flora;
namespace {
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) throw std::runtime_error("Fixture write failed");
}
QByteArray read(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Fixture read failed");
    return file.readAll();
}
QStringList entries(const QString &root) { return QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden); }
void fixture(const QString &source) {
    save(source + "/geometry.json", "{\"event\":\"200\"}");
    save(source + "/vertices.bin", QByteArray(3 * 1024 * 1024 + 17, 'v'));
}
}
class GeometryExportTests final : public QObject {
    Q_OBJECT
  private slots:
    void completeSet() {
        QTemporaryDir source, root;
        fixture(source.path());
        for (const auto name : {"vertices.csv", "custom.csv", "geometry.obj", "vertices.validity.bin",
                               "unique_vertices.bin", "patch_constants.bin", "patch_constants.validity.bin"})
            save(source.filePath(name), QByteArray(name));
        save(source.filePath("report.json"), "Not exported");
        save(source.filePath("frame.png"), "Not exported");
        const auto assets = geometryExportAssets(source.path());
        QCOMPARE(assets.size(), size_t(9));
        const auto first = exportGeometryDirectory(source.path(), assets, 200, root.path());
        QCOMPARE(QFileInfo(first).fileName(), QString("FloraGPA-Geometry-200"));
        QCOMPARE(entries(first).size(), 9);
        for (const auto &asset : assets) QCOMPARE(read(first + '/' + asset.name), read(source.filePath(asset.name)));
        // Existing complete directories and colliding files are never replaced.
        save(root.filePath("FloraGPA-Geometry-200-1"), "Keep file");
        const auto second = exportGeometryDirectory(source.path(), assets, 200, root.path());
        QCOMPARE(QFileInfo(second).fileName(), QString("FloraGPA-Geometry-200-2"));
        QCOMPARE(read(root.filePath("FloraGPA-Geometry-200-1")), QByteArray("Keep file"));
        QCOMPARE(entries(root.path()).size(), 3);
    }
    void failure_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"cancel-before", "cancel-middle", "cancel-between", "cancel-publish",
                               "missing", "shrink", "grow", "read-lock", "invalid-root", "publication-lock"})
            QTest::newRow(mode) << QString(mode);
    }
    void failure() {
        QFETCH(QString, mode);
        QTemporaryDir source, root;
        fixture(source.path());
        const auto assets = geometryExportAssets(source.path());
        QVERIFY(QDir(root.path()).mkdir("FloraGPA-Geometry-200"));
        save(root.filePath("FloraGPA-Geometry-200/keep"), "Previous export");
        HANDLE lock = INVALID_HANDLE_VALUE;
        auto release = qScopeGuard([&] { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); });
        if (mode == "missing") QVERIFY(QFile::remove(source.filePath("vertices.bin")));
        if (mode == "shrink") save(source.filePath("vertices.bin"), "short");
        if (mode == "grow") save(source.filePath("vertices.bin"), QByteArray(4 * 1024 * 1024, 'v'));
        if (mode == "read-lock") {
            lock = CreateFileW(reinterpret_cast<const wchar_t *>(source.filePath("vertices.bin").utf16()),
                GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(lock != INVALID_HANDLE_VALUE);
        }
        auto destination = root.path();
        if (mode == "invalid-root") { destination = root.filePath("file"); save(destination, "Keep"); }
        bool reached = false;
        int calls = 0;
        const auto cancelled = [&] {
            ++calls;
            if (mode == "cancel-before") { reached = true; return true; }
            if (mode == "cancel-middle" && calls == 10) { reached = true; return true; }
            const auto stages = QDir(root.path()).entryList({".FloraGPA-Geometry-*"}, QDir::Dirs | QDir::Hidden);
            if (!stages.empty()) {
                const auto stage = root.filePath(stages.first());
                const bool report = QFileInfo::exists(stage + "/geometry.json");
                const bool vertices = QFileInfo::exists(stage + "/vertices.bin");
                if ((mode == "cancel-between" && report && !vertices) || (mode == "cancel-publish" && vertices)) {
                    reached = true; return true;
                }
                if (mode == "publication-lock" && vertices && lock == INVALID_HANDLE_VALUE) {
                    lock = CreateFileW(reinterpret_cast<const wchar_t *>(stage.utf16()), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (lock == INVALID_HANDLE_VALUE) throw std::logic_error("Cannot lock staging directory");
                    reached = true;
                }
            }
            return false;
        };
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportGeometryDirectory(source.path(), assets, 200, destination, cancelled));
        if (mode.startsWith("cancel-") || mode == "publication-lock") QVERIFY(reached);
        QCOMPARE(read(root.filePath("FloraGPA-Geometry-200/keep")), QByteArray("Previous export"));
        QVERIFY(!QFileInfo::exists(root.filePath("FloraGPA-Geometry-200-1")));
        if (lock != INVALID_HANDLE_VALUE) { CloseHandle(lock); lock = INVALID_HANDLE_VALUE; }
        // An external directory lock can prevent QTemporaryDir cleanup as well
        // as publication. A hidden staging remainder is not a complete export.
        if (mode != "publication-lock" && mode != "invalid-root")
            QCOMPARE(entries(root.path()), QStringList{"FloraGPA-Geometry-200"});
        fixture(source.path());
        const auto retry = exportGeometryDirectory(source.path(), assets, 200, root.path());
        QCOMPARE(read(retry + "/vertices.bin"), read(source.filePath("vertices.bin")));
    }
    void invalidInventory_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"empty", "parent", "absolute", "stream", "duplicate", "negative", "missing-report", "zero-event"})
            QTest::newRow(mode) << QString(mode);
    }
    void invalidInventory() {
        QFETCH(QString, mode);
        QTemporaryDir source, root; fixture(source.path());
        auto assets = geometryExportAssets(source.path());
        if (mode == "empty") assets.clear();
        if (mode == "parent") assets[1].name = "../outside";
        if (mode == "absolute") assets[1].name = "C:/outside";
        if (mode == "stream") assets[1].name = "vertices.bin:stream";
        if (mode == "duplicate") assets.push_back({"GEOMETRY.JSON", 1});
        if (mode == "negative") assets[1].size = -1;
        if (mode == "missing-report") assets.erase(assets.begin());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, exportGeometryDirectory(source.path(), assets,
            mode == "zero-event" ? 0 : 200, root.path()));
        QVERIFY(entries(root.path()).empty());
    }
    void missingInventoryReport() {
        QTemporaryDir source;
        save(source.filePath("vertices.csv"), "rows");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, geometryExportAssets(source.path()));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, geometryExportAssets(source.filePath("absent")));
    }
};
QTEST_GUILESS_MAIN(GeometryExportTests)
#include "GeometryExportTests.moc"
