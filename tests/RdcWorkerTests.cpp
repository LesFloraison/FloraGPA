#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class RdcWorkerTests : public QObject {
    Q_OBJECT
    static QString worker() { return QCoreApplication::applicationDirPath() + "/FloraGPA.Rdc.exe"; }
    static void write(const QString &path, const QByteArray &bytes) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(bytes), bytes.size());
    }
    static QByteArray read(const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return f.readAll();
    }
    static int run(const QString &job) {
        QProcess p;
        p.start(worker(), {"--job", job});
        if (!p.waitForStarted(10000) || !p.waitForFinished(30000))
            return -1;
        return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    }
  private slots:
    void invalidJob_data() {
        QTest::addColumn<QJsonObject>("job");
        QTest::addColumn<QString>("error");
        QTest::newRow("unknown-action") << QJsonObject{{"action", "invented"}} << "Unsupported native";
        QTest::newRow("both-events") << QJsonObject{{"action", "history"}, {"gpa_event", 18}, {"eid", 47}}
                                     << "not both";
        QTest::newRow("negative-coordinate")
            << QJsonObject{{"action", "history"}, {"x", -1}} << "unsigned integer";
        QTest::newRow("overflow-coordinate")
            << QJsonObject{{"action", "history"}, {"y", 4294967296.0}} << "uint32";
        QTest::newRow("boolean-resource")
            << QJsonObject{{"action", "history"}, {"resource", true}} << "unsigned integer";
        QTest::newRow("counters-both-events")
            << QJsonObject{{"action", "counters"}, {"gpa_event", 18}, {"eid", 47}} << "not both";
        QTest::newRow("counters-negative-event")
            << QJsonObject{{"action", "counters"}, {"gpa_event", -1}} << "unsigned integer";
        QTest::newRow("inventory-both-events")
            << QJsonObject{{"action", "inventory"}, {"gpa_event", 18}, {"eid", 47}} << "not both";
        QTest::newRow("texture-negative-mip")
            << QJsonObject{{"action", "texture"}, {"mip", -1}} << "unsigned integer";
        QTest::newRow("texture-overflow-sample")
            << QJsonObject{{"action", "texture"}, {"sample", 4294967296.0}} << "uint32";
        QTest::newRow("debug-both-events")
            << QJsonObject{{"action", "debug-vertex"}, {"gpa_event", 18}, {"eid", 47}} << "not both";
        QTest::newRow("debug-negative-index")
            << QJsonObject{{"action", "debug-vertex"}, {"index", -1}} << "unsigned integer";
        QTest::newRow("debug-overflow-instance")
            << QJsonObject{{"action", "debug-vertex"}, {"instance", 4294967296.0}} << "uint32";
        QTest::newRow("debug-group-size")
            << QJsonObject{{"action", "debug-thread"}, {"group", QJsonArray{0, 0}}} << "three coordinates";
        QTest::newRow("debug-thread-overflow")
            << QJsonObject{{"action", "debug-thread"}, {"thread", QJsonArray{0, 4294967296.0, 0}}}
            << "uint32";
        QTest::newRow("debug-thread-boolean")
            << QJsonObject{{"action", "debug-thread"}, {"thread", QJsonArray{0, true, 0}}}
            << "unsigned integer";
        QTest::newRow("wrong-library")
            << QJsonObject{{"action", "history"},
                           {"renderdoc", qEnvironmentVariable("WINDIR") + "/System32/version.dll"}}
            << "Missing RenderDoc entry point";
    }
    void invalidJob() {
        QFETCH(QJsonObject, job);
        QFETCH(QString, error);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto out = dir.path() + QString::fromUtf8("/结果");
        job["out"] = out;
        const auto input = dir.path() + "/job.json";
        write(input, QJsonDocument(job).toJson());
        QCOMPARE(run(input), 1);
        const auto result = QJsonDocument::fromJson(read(out + "/result.json")).object();
        QCOMPARE(result["ok"].toBool(true), false);
        QVERIFY2(result["error"].toString().contains(error), qPrintable(result["error"].toString()));
    }
    void preservesExistingOutput() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto out = dir.path() + "/existing";
        QVERIFY(QDir().mkpath(out));
        const QByteArray original("existing result must survive");
        write(out + "/result.json", original);
        const auto input = dir.path() + "/job.json";
        write(input, QJsonDocument(QJsonObject{{"out", out}, {"action", "history"}}).toJson());
        QCOMPARE(run(input), 1);
        QCOMPARE(read(out + "/result.json"), original);
    }
    void rejectsMalformedOrOversizedJob() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto input = dir.path() + "/job.json";
        write(input, "{broken");
        QCOMPARE(run(input), 1);
        write(input, QByteArray(1048577, ' '));
        QCOMPARE(run(input), 1);
    }
};
QTEST_GUILESS_MAIN(RdcWorkerTests)
#include "RdcWorkerTests.moc"
