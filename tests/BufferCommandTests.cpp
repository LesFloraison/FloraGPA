#include "MsaaCapture.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
namespace {
QByteArray load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read buffer result");
    return file.readAll();
}
void fixture(const QString &path, const QByteArray &bytes) {
    auto c = testing::msaaOutputCapture(false);
    std::vector<uint8_t> resource(16);
    testing::append(resource, D3D11_BUFFER_DESC{UINT(bytes.size()), D3D11_USAGE_DEFAULT,
        D3D11_BIND_VERTEX_BUFFER, 0, 0, 0});
    testing::append(resource, Id(81)); c.add(80, 5, 0x83, resource);
    auto data = testing::word(UINT(bytes.size()));
    data.insert(data.end(), bytes.constData(), bytes.constData() + bytes.size());
    c.add(81, 9, 1, std::move(data)); c.save(path);
}
}
class BufferCommandTests final : public QObject {
    Q_OBJECT
  private slots:
    void parity_data() {
        QTest::addColumn<QString>("exe"); QTest::addColumn<QString>("mode");
        for (auto exe : {"FloraGPA.Cli", "FloraGPA.Worker"})
            for (auto mode : {"initial", "before", "after", "unaligned", "empty"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(exe, mode))) << QString(exe) << QString(mode);
    }
    void parity() {
        QFETCH(QString, exe); QFETCH(QString, mode);
        QTemporaryDir root;
        const auto capture = root.filePath("frame.gpa_frame");
        const auto bytes = QByteArray::fromHex("0000803f00000000000000807faabbccdd");
        fixture(capture, bytes);
        QStringList args{"buffer", capture, "--id", "80", "--warp"};
        if (mode == "before" || mode == "after") args << "--event" << "100";
        if (mode == "before") args << "--before";
        if (mode == "unaligned") args << "--offset" << "3" << "--length" << "7";
        if (mode == "empty") args << "--offset" << QString::number(bytes.size());
        QJsonObject reports[2];
        for (int skip = 0; skip < 2; ++skip) {
            const auto out = root.filePath(QString::number(skip));
            auto command = args; command << "--out" << out;
            if (skip) command << "--no-buffer-csv";
            QProcess process;
            process.start(QCoreApplication::applicationDirPath() + '/' + exe + ".exe", command);
            QVERIFY(process.waitForFinished(30000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
            QCOMPARE(load(out + "/buffer.bin"), mode == "empty" ? QByteArray() :
                     mode == "unaligned" ? bytes.mid(3, 7) : bytes);
            QCOMPARE(QFileInfo::exists(out + "/words.csv"), !skip);
            reports[skip] = QJsonDocument::fromJson(load(out + "/report.json")).object();
            QVERIFY(reports[skip]["completed"].toBool());
            // Windows may load the same modules in a different order in each
            // child. Compare their complete sorted paths, not loader order.
            QStringList modules;
            for (const auto &module : reports[skip]["loaded_modules"].toArray()) modules << module.toString();
            QVERIFY(!modules.empty()); modules.sort();
            reports[skip]["loaded_modules"] = QJsonArray::fromStringList(modules);
            if (!skip) {
                const auto csv = load(out + "/words.csv");
                const QByteArray header = "byte_offset,hex_bytes,uint32,int32,float32\n";
                QVERIFY(csv.startsWith(header));
                if (mode == "empty") QCOMPARE(csv, header);
                else if (mode != "unaligned") {
                    QVERIFY(csv.contains("0,0000803f,1065353216,1065353216,1\n"));
                    QVERIFY(csv.endsWith("16,dd,,,\n"));
                }
            }
        }
        QCOMPARE(reports[0], reports[1]);
    }
    void invalid_data() {
        QTest::addColumn<QString>("mode");
        for (auto mode : {"wrong-command", "wrong-resource", "overrun", "bad-length", "truncated"})
            QTest::newRow(mode) << QString(mode);
    }
    void largeRead() {
        QTemporaryDir root; const auto capture = root.filePath("large.gpa_frame");
        const QByteArray bytes(64 * 1024 * 1024, 'C');
        fixture(capture, bytes);
        const auto out = root.filePath("result");
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Worker.exe",
            {"buffer", capture, "--id", "80", "--out", out, "--no-buffer-csv"});
        QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
        QCOMPARE(load(out + "/buffer.bin"), bytes);
        QVERIFY(!QFileInfo::exists(out + "/words.csv"));
        const auto report = QJsonDocument::fromJson(load(out + "/report.json")).object();
        QVERIFY(report["completed"].toBool());
        QCOMPARE(report["length"].toInteger(), qint64(bytes.size()));
        const auto artifacts = qEnvironmentVariable("FLORA_BUFFER_COMMAND_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(QFile::copy(capture, artifacts + "/large.gpa_frame"));
        }
    }
    void invalid() {
        QFETCH(QString, mode);
        QTemporaryDir root; const auto capture = root.filePath("frame.gpa_frame");
        fixture(capture, QByteArray(16, 'x'));
        if (mode == "truncated") { QFile f(capture); QVERIFY(f.open(QIODevice::ReadWrite)); QVERIFY(f.resize(24)); }
        for (auto exe : {"FloraGPA.Cli", "FloraGPA.Worker"}) {
            QByteArray previous;
            for (int skip = 0; skip < 2; ++skip) {
                auto out = root.filePath(QString("%1-%2").arg(exe).arg(skip));
                QStringList args{mode == "wrong-command" ? "inventory" : "buffer", capture,
                    "--id", mode == "wrong-resource" ? "20" : "80", "--warp", "--out", out};
                if (skip || mode == "wrong-command") args << "--no-buffer-csv";
                if (mode == "overrun") args << "--offset" << "16" << "--length" << "1";
                if (mode == "bad-length") args << "--length" << "-1";
                QProcess process; process.start(QCoreApplication::applicationDirPath() + '/' + exe + ".exe", args);
                QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitStatus(), QProcess::NormalExit);
                QCOMPARE(process.exitCode(), 1);
                const auto error = process.readAllStandardError();
                QVERIFY(!error.isEmpty()); if (skip) QCOMPARE(error, previous); previous = error;
                QVERIFY(!QFileInfo::exists(out + "/words.csv"));
                QVERIFY(!QFileInfo::exists(out + "/buffer.bin"));
                if (mode == "wrong-command") QVERIFY(error.contains("--no-buffer-csv applies to buffer only"));
            }
        }
    }
};
QTEST_GUILESS_MAIN(BufferCommandTests)
#include "BufferCommandTests.moc"
