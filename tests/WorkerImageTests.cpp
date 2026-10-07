#include "app/WorkerImage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <Windows.h>
using namespace flora;
class WorkerImageTests final : public QObject {
    Q_OBJECT
  private slots:
    void cancellation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // Cross a raw hashing chunk boundary and include RGB-to-RGBA conversion.
        QImage image(131073, 3, QImage::Format_RGB888);
        image.fill(QColor(17, 38, 91));
        QVERIFY(image.save(dir.filePath("frame.png")));
        const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
        QByteArray bytes;
        for (int y = 0; y < rgba.height(); ++y)
            bytes.append(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
        QFile raw(dir.filePath("frame.rgba"));
        QVERIFY(raw.open(QIODevice::WriteOnly));
        QCOMPARE(raw.write(bytes), bytes.size());
        raw.close();
        QJsonObject report{
            {"image_available", true},
            {"width", image.width()},
            {"height", image.height()},
            {"rgba_sha256",
             QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        unsigned checks = 0;
        QCOMPARE(readWorkerImage(dir.path(), report, true,
                                 [&] {
                                     ++checks;
                                     return false;
                                 }),
                 rgba);
        QVERIFY(checks >= 10);
        for (unsigned stop = 1; stop <= checks; ++stop) {
            unsigned at = 0;
            QVERIFY_EXCEPTION_THROWN(readWorkerImage(dir.path(), report, true, [&] { return ++at == stop; }),
                                     OperationCancelled);
            QCOMPARE(readWorkerImage(dir.path(), report, true), rgba);
        }
        qInfo() << "Image interruption/retry positions" << checks;
    }
    void artifacts_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"rgba",
                                "rgb",
                                "gray",
                                "no-output",
                                "zero",
                                "negative",
                                "fraction",
                                "string",
                                "oversize",
                                "missing-width",
                                "missing-hash",
                                "invalid-hash",
                                "availability-string",
                                "availability-missing",
                                "no-output-metadata",
                                "no-output-file",
                                "short-raw",
                                "long-raw",
                                "wrong-raw",
                                "wrong-png",
                                "missing-raw",
                                "locked-raw",
                                "missing-png",
                                "truncated-png",
                                "locked-png"})
            QTest::newRow(mode) << QString(mode);
    }
    void artifacts() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage image(3, 2,
                     mode == "gray"  ? QImage::Format_Grayscale8
                     : mode == "rgb" ? QImage::Format_RGB888
                                     : QImage::Format_RGBA8888);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor(3 + x * 70, 19 + y * 90, 211 - x * 40, x * 127));
        const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
        QByteArray bytes;
        for (int y = 0; y < rgba.height(); ++y)
            bytes.append(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
        QJsonObject report{
            {"width", 3},
            {"height", 2},
            {"image_available", true},
            {"rgba_sha256",
             QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        if (mode.startsWith("no-output")) {
            report = {{"image_available", false},
                      {"width", QJsonValue::Null},
                      {"height", QJsonValue::Null},
                      {"resource", QJsonValue::Null},
                      {"rgba_sha256", QJsonValue::Null}};
            if (mode == "no-output-metadata")
                report["width"] = 3;
        }
        if (mode == "zero")
            report["width"] = 0;
        if (mode == "negative")
            report["width"] = -1;
        if (mode == "fraction")
            report["width"] = 3.5;
        if (mode == "string")
            report["width"] = "3";
        if (mode == "oversize")
            report["width"] = double(UINT64_MAX);
        if (mode == "missing-width")
            report.remove("width");
        if (mode == "missing-hash")
            report.remove("rgba_sha256");
        if (mode == "invalid-hash")
            report["rgba_sha256"] = QString(64, 'z');
        if (mode == "availability-string")
            report["image_available"] = "false";
        if (mode == "availability-missing")
            report.remove("image_available");
        if (mode == "short-raw")
            bytes.chop(1);
        if (mode == "long-raw")
            bytes.append('x');
        if (mode == "wrong-raw")
            bytes[0] ^= 0xff;
        if (!mode.startsWith("no-output") || mode == "no-output-file") {
            if (mode != "missing-raw") {
                QFile file(dir.filePath("frame.rgba"));
                QVERIFY(file.open(QIODevice::WriteOnly));
                QCOMPARE(file.write(bytes), bytes.size());
            }
            if (mode != "missing-png") {
                if (mode == "wrong-png")
                    image.setPixelColor(1, 1, Qt::red);
                QVERIFY(image.save(dir.filePath("frame.png")));
                if (mode == "truncated-png") {
                    QFile file(dir.filePath("frame.png"));
                    QVERIFY(file.open(QIODevice::ReadWrite));
                    QVERIFY(file.resize(40)); // Retain the header, lose the pixel payload.
                }
            }
        }
        HANDLE lock = INVALID_HANDLE_VALUE;
        if (mode.startsWith("locked-")) {
            const auto path = dir.filePath(mode == "locked-raw" ? "frame.rgba" : "frame.png").toStdWString();
            lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            QVERIFY(lock != INVALID_HANDLE_VALUE);
        }
        auto unlock = qScopeGuard([&] {
            if (lock != INVALID_HANDLE_VALUE)
                CloseHandle(lock);
        });
        const bool valid = mode == "rgba" || mode == "rgb" || mode == "gray" || mode == "no-output";
        if (valid) {
            const auto actual = readWorkerImage(dir.path(), report, true);
            if (mode == "no-output")
                QVERIFY(actual.isNull());
            else {
                QCOMPARE(actual, rgba);
                // Texture jobs use the same artifact contract without the
                // replay-only no-output marker.
                report.remove("image_available");
                QCOMPARE(readWorkerImage(dir.path(), report, false), rgba);
            }
        } else {
            QVERIFY_EXCEPTION_THROWN(readWorkerImage(dir.path(), report, true), std::runtime_error);
        }
    }
};
QTEST_GUILESS_MAIN(WorkerImageTests)
#include "WorkerImageTests.moc"
