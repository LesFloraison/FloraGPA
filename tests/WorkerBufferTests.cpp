#include "app/WorkerBuffer.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
namespace {
QJsonObject reportFor(const BufferRequest &request, const QByteArray &bytes) {
    return {{"completed", true}, {"resource", QString::number(request.resource)},
            {"offset", qint64(request.offset)}, {"length", qint64(request.length)},
            {"event", request.event ? QJsonValue(QString::number(request.event)) : QJsonValue(QJsonValue::Null)},
            {"value_time", request.event ? (request.before ? "before_event" : "after_event") : "capture_initial"},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}
void save(const QString &directory, const QByteArray &bytes) {
    QFile file(directory + "/buffer.bin");
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
}
class WorkerBufferTests final : public QObject {
    Q_OBJECT
  private slots:
    void valid_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("initial-unaligned") << 0;
        QTest::newRow("before-event") << 1;
        QTest::newRow("after-event") << 2;
        QTest::newRow("empty-at-end") << 3;
        QTest::newRow("multiple-chunks") << 4;
    }
    void valid() {
        QFETCH(int, mode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray bytes(mode == 3 ? 0 : mode == 4 ? 2 * 1024 * 1024 + 17 : 17, Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = char(i * 31 + i / 257);
        BufferRequest request{UINT64_MAX - 1, 3, uint64_t(bytes.size()), mode == 1 || mode == 2 ? UINT64_MAX - 2 : 0, mode == 1};
        save(dir.path(), bytes);
        QCOMPARE(readWorkerBuffer(dir.path(), reportFor(request, bytes), request), bytes);
    }
    void invalid_data() {
        QTest::addColumn<QString>("mode");
        for (auto mode : {"missing", "short", "long", "hash", "hash-missing", "hash-invalid",
                          "resource", "resource-number", "offset", "length", "negative", "fraction",
                          "overflow", "string-length", "event", "event-missing", "boundary", "completed",
                          "request-overflow", "request-zero-resource", "resize-during-read"})
            QTest::newRow(mode) << QString(mode);
    }
    void invalid() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray bytes(2 * 1024 * 1024 + 17, 'x');
        BufferRequest request{UINT64_MAX - 1, 3, uint64_t(bytes.size()), 101, true};
        auto report = reportFor(request, bytes);
        if (mode == "short") bytes.chop(1);
        if (mode == "long") bytes.append('x');
        if (mode == "hash") bytes[0] ^= 1;
        if (mode == "hash-missing") report.remove("sha256");
        if (mode == "hash-invalid") report["sha256"] = QString(64, 'z');
        if (mode == "resource") report["resource"] = "99";
        if (mode == "resource-number") report["resource"] = 80;
        if (mode == "offset") report["offset"] = 4;
        if (mode == "length") report["length"] = 4;
        if (mode == "negative") report["length"] = -1;
        if (mode == "fraction") report["offset"] = 3.5;
        if (mode == "overflow") report["offset"] = 4294967296.;
        if (mode == "string-length") report["length"] = QString::number(bytes.size());
        if (mode == "event") report["event"] = "102";
        if (mode == "event-missing") report.remove("event");
        if (mode == "boundary") report["value_time"] = "after_event";
        if (mode == "completed") report["completed"] = "true";
        if (mode == "request-overflow") request.offset = UINT64_MAX;
        if (mode == "request-zero-resource") request.resource = 0;
        if (mode != "missing") save(dir.path(), bytes);
        unsigned calls = 0;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readWorkerBuffer(dir.path(), report, request, [&] {
            if (++calls == 3 && mode == "resize-during-read") {
                QFile file(dir.filePath("buffer.bin"));
                if (file.open(QIODevice::ReadWrite)) file.resize(1);
            }
            return false;
        }));
        // Every rejection must permit reading a correct artifact on retry.
        save(dir.path(), bytes);
        request = {80, 3, uint64_t(bytes.size())};
        QCOMPARE(readWorkerBuffer(dir.path(), reportFor(request, bytes), request), bytes);
    }
    void cancellation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray bytes(2 * 1024 * 1024 + 17, 'c');
        const BufferRequest request{80, 3, uint64_t(bytes.size())};
        const auto report = reportFor(request, bytes);
        save(dir.path(), bytes);
        unsigned checks = 0;
        QCOMPARE(readWorkerBuffer(dir.path(), report, request, [&] { ++checks; return false; }), bytes);
        QVERIFY(checks >= 5);
        for (unsigned stop = 1; stop <= checks; ++stop) {
            unsigned at = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, readWorkerBuffer(dir.path(), report, request, [&] { return ++at == stop; }));
            QCOMPARE(readWorkerBuffer(dir.path(), report, request), bytes);
        }
        qInfo() << "Buffer cancellation/retry positions" << checks;
    }
};
QTEST_GUILESS_MAIN(WorkerBufferTests)
#include "WorkerBufferTests.moc"
