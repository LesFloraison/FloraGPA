#include "app/WorkerReport.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
namespace {
void save(const QString &directory, const QByteArray &bytes) {
    QFile file(directory + "/report.json");
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
}
class WorkerReportTests final : public QObject {
    Q_OBJECT
  private slots:
    void analyzerPayload_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("filename");
        for (const auto &pair : {qMakePair("quad", "quad.json"), qMakePair("coverage", "coverage.json"),
             qMakePair("timings", "profile.json"), qMakePair("statistics", "statistics.json"),
             qMakePair("replay-pipeline", "replay-pipeline.json"), qMakePair("predicate", "predicate.json"),
             qMakePair("geometry", "geometry.json"), qMakePair("post-geometry", "geometry-ui.json")})
            QTest::newRow(pair.first) << QString(pair.first) << QString(pair.second);
    }
    void analyzerPayload() {
        QFETCH(QString, kind);
        QFETCH(QString, filename);
        QTemporaryDir dir;
        save(dir.path(), "{\"completed\":true}");
        auto write = [&](const QByteArray &bytes) {
            QFile file(dir.filePath(filename));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), bytes.size());
        };
        // Completed envelope cannot conceal a missing, truncated or non-object payload.
        QVERIFY_THROWS_EXCEPTION(std::exception, readWorkerOutput(dir.path(), kind));
        const QByteArray good("{\"event\":\"18446744073709551615\",\"counter\":18446744073709551615,\"signed\":-9223372036854775808,\"items\":[1,2,3]}");
        for (qsizetype n = 0; n < good.size(); ++n) {
            write(good.left(n));
            QVERIFY_THROWS_EXCEPTION(std::exception, readWorkerOutput(dir.path(), kind));
        }
        for (const auto &bad : {QByteArray("[]"), QByteArray("true"), good + 'x',
                               QByteArray("{\"x\":") + QByteArray(1100, '[') + '0' + QByteArray(1100, ']') + '}',
                               QByteArray("{\"s\":\"") + char(-1) + "\"}"}) {
            write(bad);
            try {
                readWorkerOutput(dir.path(), kind);
                QFAIL("Invalid analyzer payload accepted");
            } catch (const std::exception &error) {
                QVERIFY2(QString::fromUtf8(error.what()).contains(filename), error.what());
            }
        }
        write(good);
        auto accepted = readWorkerOutput(dir.path(), kind);
        if (kind.contains("geometry")) {
            QCOMPARE(accepted.geometry["event"].toString(), QString("18446744073709551615"));
            QCOMPARE(accepted.geometry["items"].toArray().size(), 3);
            QVERIFY(accepted.payload.is_null());
        } else {
            QCOMPARE(accepted.payload.at("counter").get<uint64_t>(), UINT64_MAX);
            QCOMPARE(accepted.payload.at("signed").get<int64_t>(), INT64_MIN);
        }
        const auto large = QByteArray("{\"padding\":\"") + QByteArray(2*1024*1024+17, 'x') +
                           "\",\"items\":[" + QByteArray("1,").repeated(1024) + "2]}";
        write(large);
        int checks = 0;
        readWorkerOutput(dir.path(), kind, [&] { ++checks; return false; });
        QVERIFY(checks > 10);
        for (int stop = 1; stop <= checks; ++stop) {
            int at = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                readWorkerOutput(dir.path(), kind, [&] { return ++at == stop; }));
        }
        write(good);
        accepted = readWorkerOutput(dir.path(), kind);
        QVERIFY(!accepted.geometry.isEmpty() || accepted.payload.is_object());
    }
    void valid_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("replay-export-integers") << 0;
        QTest::newRow("metadata-only") << 1;
        QTest::newRow("no-output") << 2;
        QTest::newRow("utf8-bom") << 3;
    }
    void valid() {
        QFETCH(int, mode);
        QTemporaryDir dir;
        auto bytes = QByteArray("{\"completed\":true,\"resource\":\"18446744073709551615\",\"integer\":18446744073709551615,\"signed\":-9223372036854775808,\"text\":\"\\u6708\\u4e0b\",\"nested\":{\"x\":[true,null,1.25]}}");
        if (mode == 2) bytes.insert(bytes.size()-1, ",\"image_available\":false");
        if (mode == 3) bytes.prepend(QByteArray::fromHex("efbbbf"));
        save(dir.path(), bytes);
        const auto result = readWorkerReport(dir.path(), mode != 1);
        QCOMPARE(result.report["resource"].toString(), QString("18446744073709551615"));
        QCOMPARE(result.report["text"].toString(), QString::fromUtf8("月下"));
        if (mode == 1 || mode == 2) QVERIFY(result.replayReport.is_null());
        else {
            QCOMPARE(result.replayReport.at("integer").get<uint64_t>(), UINT64_MAX);
            QCOMPARE(result.replayReport.at("signed").get<int64_t>(), INT64_MIN);
            QCOMPARE(result.replayReport.at("nested").at("x").at(2).get<double>(), 1.25);
        }
    }
    void invalid_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("array-root") << QByteArray("[]");
        QTest::newRow("scalar-root") << QByteArray("true");
        QTest::newRow("syntax") << QByteArray("{broken");
        QTest::newRow("trailing") << QByteArray("{\"completed\":true}garbage");
        QTest::newRow("unfinished") << QByteArray("{\"completed\":false}");
        QTest::newRow("missing-completed") << QByteArray("{}");
        QTest::newRow("string-completed") << QByteArray("{\"completed\":\"true\"}");
        QTest::newRow("number-completed") << QByteArray("{\"completed\":1}");
        QTest::newRow("number-overflow") << QByteArray("{\"completed\":true,\"n\":1e400}");
        QTest::newRow("invalid-utf8") << (QByteArray("{\"completed\":true,\"s\":\"") + char(-1) + "\"}");
        QTest::newRow("nesting-limit") << (QByteArray("{\"completed\":true,\"v\":") + QByteArray(1100, '[') + '0' + QByteArray(1100, ']') + '}');
    }
    void invalid() {
        QFETCH(QByteArray, bytes);
        QTemporaryDir dir;
        save(dir.path(), bytes);
        QVERIFY_THROWS_EXCEPTION(std::exception, readWorkerReport(dir.path(), true));
    }
    void missingAndTruncated() {
        QTemporaryDir dir;
        QVERIFY_THROWS_EXCEPTION(std::exception, readWorkerReport(dir.path(), true));
        const QByteArray bytes("{\"completed\":true,\"nested\":[{\"event\":\"18446744073709551615\",\"size\":1234}]}");
        for (qsizetype n=0; n<bytes.size(); ++n) {
            save(dir.path(), bytes.left(n));
            QVERIFY_THROWS_EXCEPTION(std::exception, readWorkerReport(dir.path(), true));
        }
        save(dir.path(), bytes);
        QVERIFY(readWorkerReport(dir.path(), true).report["completed"].toBool());
    }
    void cancellationAndRetry() {
        QTemporaryDir dir;
        const auto bytes = QByteArray("{\"completed\":true,\"padding\":\"") + QByteArray(2*1024*1024+17, 'x') + "\",\"events\":[" + QByteArray("1,").repeated(1024) + "2]}";
        save(dir.path(), bytes);
        int checks = 0;
        const auto expected = readWorkerReport(dir.path(), true, [&] { ++checks; return false; });
        QVERIFY(checks > 8); // Includes chunk boundaries and native parser callbacks.
        for (int stop=1; stop<=checks; ++stop) {
            int current = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, readWorkerReport(dir.path(), true, [&] { return ++current == stop; }));
        }
        const auto retry = readWorkerReport(dir.path(), true);
        QVERIFY(retry.report == expected.report && retry.replayReport == expected.replayReport);
    }
    void changedLength() {
        QTemporaryDir dir;
        save(dir.path(), "{\"completed\":true}");
        int checks = 0;
        QVERIFY_THROWS_EXCEPTION(std::exception, readWorkerReport(dir.path(), true, [&] {
            if (++checks == 2) {
                QFile file(dir.filePath("report.json"));
                if (!file.open(QIODevice::Append) || file.write(" ") != 1) qFatal("Cannot change report fixture");
            }
            return false;
        }));
    }
};
QTEST_GUILESS_MAIN(WorkerReportTests)
#include "WorkerReportTests.moc"
