#include "app/DiagnosticOutput.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
void save(const QString &root, const QString &name, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(root + '/' + name).path());
    QFile file(root + '/' + name);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray png(int width = 2, int height = 1) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(QColor(53, 97, 179, 112));
    image.setPixelColor(0, 0, QColor(0, 31, 89, 0));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) qFatal("Cannot create PNG fixture");
    return bytes;
}
Json coverage(const QString &root, const char *kind = "viewport") {
    Json shape{{"width", 2}, {"height", 1}};
    Json report{{"target_kind", kind}, {"counter", UINT64_MAX}};
    if (std::string(kind) == "viewport") report.update(shape);
    else report[std::string(kind) == "buffer" ? "target_buffer_view" : "target_subresource"] = shape;
    save(root, coverageOutputFiles[0], QByteArray::fromStdString(report.dump()));
    for (size_t i = 1; i < coverageOutputFiles.size(); ++i) save(root, coverageOutputFiles[i], png());
    return report;
}
Json quad(const QString &root) {
    Json report{{"width", 3}, {"height", 2}, {"quad_width", 2}, {"quad_height", 1},
                {"histogram_capacity", 4}, {"counter", UINT64_MAX}};
    save(root, "quad.json", QByteArray::fromStdString(report.dump()));
    for (size_t i = 0; i < 5; ++i) save(root, quadOutputFiles[i], QByteArray(i < 3 ? 8 : 16, char(i + 1)));
    save(root, quadOutputFiles[5], png());
    return report;
}
}
class DiagnosticOutputTests final : public QObject {
    Q_OBJECT
  private slots:
    void coverageCoordinates_data() {
        QTest::addColumn<QByteArray>("kind");
        for (const auto kind : {"viewport", "color", "depth", "buffer"})
            QTest::newRow(kind) << QByteArray(kind);
    }
    void coverageCoordinates() {
        QFETCH(QByteArray, kind);
        QTemporaryDir dir;
        const auto expected = coverage(dir.path(), kind.constData());
        const auto result = readCoverageOutput(dir.path());
        QVERIFY(result.report == expected);
        QCOMPARE(result.report.at("counter").get<uint64_t>(), UINT64_MAX);
        QCOMPARE(result.files[0], QByteArray::fromStdString(expected.dump()));
        for (size_t i = 1; i < result.files.size(); ++i) QCOMPARE(result.files[i], png());
        QCOMPARE(result.mask.size(), QSize(2, 1));
        QCOMPARE(result.maskDisplay.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(result.maskDisplay.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(result.maskDisplay.pixelColor(1, 0), QColor(255, 0, 255));
        QCOMPARE(result.diagnostic.original.pixelColor(1, 0), QColor(53, 97, 179, 112));
        QCOMPARE(result.diagnostic.display.pixelColor(1, 0), QColor(53, 97, 179));
        QCOMPARE(result.diagnostic.display.pixelColor(0, 0), QColor(0, 31, 89));
    }
    void missingCorruptAndMismatchedPng() {
        QTemporaryDir dir;
        for (const bool isQuad : {false, true}) {
            if (isQuad) quad(dir.path()); else coverage(dir.path());
            const auto name = QString(isQuad ? quadOutputFiles[5] : coverageOutputFiles[3]);
            auto read = [&] { if (isQuad) readQuadOutput(dir.path(), "key"); else readCoverageOutput(dir.path()); };
            QVERIFY(QFile::remove(dir.filePath(name)));
            QVERIFY_THROWS_EXCEPTION(std::exception, read());
            for (const auto &bad : {QByteArray(), QByteArray("not png"), png().left(png().size()/2), png(1,1), png(2,2)}) {
                save(dir.path(), name, bad);
                QVERIFY_THROWS_EXCEPTION(std::exception, read());
            }
            save(dir.path(), name, png());
            read();
        }
        for (size_t i = 1; i < coverageOutputFiles.size(); ++i) {
            coverage(dir.path());
            save(dir.path(), coverageOutputFiles[i], png(3, 1));
            QVERIFY_THROWS_EXCEPTION(std::exception, readCoverageOutput(dir.path()));
        }
    }
    void quadStorageAndOverflow() {
        QTemporaryDir dir;
        const auto original = quad(dir.path());
        auto result = readQuadOutput(dir.path(), "experiment:0");
        auto expected = original; expected["experiment_key"] = "experiment:0";
        QVERIFY(result.report == expected);
        QVERIFY(Json::parse(result.reportText.toStdString()) == expected);
        QCOMPARE(result.image.original.pixelColor(1,0).alpha(), 112);
        QCOMPARE(result.image.display.pixelColor(1,0).alpha(), 255);
        for (size_t file = 0; file < 5; ++file) {
            const int full = file < 3 ? 8 : 16;
            for (int length = 0; length <= full + 1; ++length) {
                if (length == full) continue;
                save(dir.path(), quadOutputFiles[file], QByteArray(length, 0));
                QVERIFY_THROWS_EXCEPTION(std::exception, readQuadOutput(dir.path(), "key"));
            }
            quad(dir.path());
        }
        for (const auto &invalid : {Json(uint64_t(1) << 62), Json(UINT64_MAX), Json(-1), Json(0), Json(1.5)}) {
            auto report = original; report["histogram_capacity"] = invalid;
            save(dir.path(), "quad.json", QByteArray::fromStdString(report.dump()));
            // 2^62 * 4 wraps to zero in an unchecked uint64_t multiplication.
            save(dir.path(), quadOutputFiles[3], {});
            QVERIFY_THROWS_EXCEPTION(std::exception, readQuadOutput(dir.path(), "key"));
        }
        quad(dir.path());
        QCOMPARE(readQuadOutput(dir.path(), "retry").report.at("counter").get<uint64_t>(), UINT64_MAX);
    }
    void invalidDimensions() {
        QTemporaryDir dir;
        for (const auto &invalid : {Json(-1), Json(0), Json(0.5), Json(UINT64_MAX), Json("2")}) {
            auto report = coverage(dir.path()); report["width"] = invalid;
            save(dir.path(), "coverage.json", QByteArray::fromStdString(report.dump()));
            QVERIFY_THROWS_EXCEPTION(std::exception, readCoverageOutput(dir.path()));
            report = quad(dir.path()); report["quad_width"] = invalid;
            save(dir.path(), "quad.json", QByteArray::fromStdString(report.dump()));
            QVERIFY_THROWS_EXCEPTION(std::exception, readQuadOutput(dir.path(), "key"));
        }
        auto report = quad(dir.path()); report["width"] = 5;
        save(dir.path(), "quad.json", QByteArray::fromStdString(report.dump()));
        QVERIFY_THROWS_EXCEPTION(std::exception, readQuadOutput(dir.path(), "key"));
    }
    void cancellationAndRetry() {
        QTemporaryDir dir;
        for (const bool isQuad : {false, true}) {
            if (isQuad) quad(dir.path()); else coverage(dir.path());
            auto read = [&](const CancelCheck &cancel) {
                if (isQuad) readQuadOutput(dir.path(), "key", cancel);
                else readCoverageOutput(dir.path(), cancel);
            };
            int checks = 0; read([&] { ++checks; return false; });
            QVERIFY(checks > 20);
            for (int stop = 1; stop <= checks; ++stop) {
                int at = 0;
                QVERIFY_THROWS_EXCEPTION(OperationCancelled, read([&] { return ++at == stop; }));
            }
            read({});
        }
    }
};
QTEST_GUILESS_MAIN(DiagnosticOutputTests)
#include "DiagnosticOutputTests.moc"
