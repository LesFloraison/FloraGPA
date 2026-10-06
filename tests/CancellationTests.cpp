#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

using namespace flora;
class CancellationTests final : public QObject {
    Q_OBJECT
    template <class F> bool cancelled(F f) {
        try { f(); }
        catch (const OperationCancelled &) { return true; }
        return false;
    }
  private slots:
    void parseCancellationReleasesMapping() {
        QTemporaryDir dir;
        const auto path = dir.filePath("many.gpa_frame");
        testing::Capture c;
        for (Id id = 1; id <= 4096; ++id)
            c.add(id, 9, 1, testing::word(0));
        c.save(path);
        DWORD before{}, after{};
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &before));
        for (int repeat = 0; repeat < 100; ++repeat) {
            unsigned calls = 0;
            QVERIFY(cancelled([&] { Frame frame(path.toStdWString(), [&] { return ++calls == 3; }); }));
            QCOMPARE(calls, 3u); // Initial checkpoint, then entries 0 and 1024.
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite)); // No abandoned read-only mapping/handle.
        }
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &after));
        QCOMPARE(after, before);
        Frame frame(path.toStdWString());
        QCOMPARE(frame.entries().size(), size_t(4096));
    }
    void hashCancellationAndRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("large.gpa_frame");
        testing::Capture c;
        c.add(1, 9, 1, testing::word(0));
        c.save(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(48 * 1024 * 1024 + 17));
        const auto bytes = file.readAll();
        const auto expected = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        file.close();
        Frame frame(path.toStdWString());
        unsigned calls = 0;
        QVERIFY(cancelled([&] { frame.sha256([&] { return ++calls == 4; }); }));
        QCOMPARE(calls, 4u); // Stop after one 16 MiB block, before consuming the rest.
        QCOMPARE(QString::fromStdString(frame.sha256()), QString::fromLatin1(expected));
        QVERIFY(cancelled([&] { frame.sha256([] { return true; }); }));
        QCOMPARE(QString::fromStdString(frame.sha256()), QString::fromLatin1(expected));
        QCOMPARE(QString::fromStdString(sha256({})), QString("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }
    void preflightCancellationIsNotCorruption() {
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        testing::Capture c;
        c.add(1, 9, 1, testing::word(0));
        c.save(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(32 * 1024 * 1024));
        file.close();
        unsigned calls = 0;
        auto report = validateFrame(path.toStdWString(), [&] { return ++calls == 7; });
        QCOMPARE(report["status"], nlohmann::json("cancelled"));
        QCOMPARE(report["completed"], nlohmann::json(false));
        QCOMPARE(report["errors"], nlohmann::json(0));
        QVERIFY(report["findings"].empty());
        QVERIFY(!report.contains("source_sha256"));
        QCOMPARE(validateFrame(path.toStdWString())["status"], nlohmann::json("checked"));
        auto absent = validateFrame(dir.filePath("absent").toStdWString(), [] { return true; });
        QCOMPARE(absent["status"], nlohmann::json("cancelled"));
        QCOMPARE(absent["errors"], nlohmann::json(0));
    }
};
QTEST_GUILESS_MAIN(CancellationTests)
#include "CancellationTests.moc"
