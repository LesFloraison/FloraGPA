#include "application/RdcJobs.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cstdio>
#include <limits>
#define NOMINMAX
#include <Windows.h>
using Json = nlohmann::json;
using namespace flora;
namespace {
void write(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write job fixture");
}
QByteArray read(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read job fixture");
    return f.readAll();
}
Json prepare(const QString &root, const QString &mode, const Json &options = Json::object()) {
    const auto capture = root + "/capture & 测试.rdc";
    const auto library = root + "/library.dll";
    write(capture, mode.toUtf8());
    write(library, "library");
    return prepareRdcJob(capture, "inventory", root + "/output & 测试", library, options);
}
int mockWorker(const Json &job) {
    const auto out = QString::fromStdString(job.at("out").get<std::string>());
    const auto mode = read(QString::fromStdString(job.at("capture").get<std::string>()));
    fprintf(stdout, "mock stdout");
    fflush(stdout);
    fprintf(stderr, "mock stderr");
    fflush(stderr);
    if (mode == "timeout") {
        QProcess child;
        child.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--delayed-child", out + "/child-survived.txt"});
        qint64 pid{};
        if (!child.startDetached(&pid))
            return 9;
        write(out + "/child-pid.txt", QByteArray::number(pid));
        QThread::msleep(10000);
    }
    if (mode == "missing")
        return 0;
    if (mode == "bad")
        write(out + "/result.json", "{broken");
    else if (mode == "failed")
        write(out + "/result.json", "{\"ok\":false,\"error\":\"fixture failure\"}");
    else if (mode == "numeric-ok")
        write(out + "/result.json", "{\"ok\":1}");
    else
        write(out + "/result.json",
              "{\"ok\":true,\"annotation\":\"\\udc80\",\"value\":18446744073709551615}");
    return mode == "exit" ? 2 : 0;
}
} // namespace
class RdcJobTests : public QObject {
    Q_OBJECT
  private slots:
    void preparation() {
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), "success", {{"resource", UINT64_MAX}, {"group", {1, 2, 3}}});
        QCOMPARE(job.at("resource"), Json(UINT64_MAX));
        QCOMPARE(job.at("index"), Json(nullptr));
        QCOMPARE(job.at("stage"), Json("VSOut"));
        QCOMPARE(job.at("group"), Json::array({1, 2, 3}));
        QCOMPARE(job.at("thread"), Json::array({0, 0, 0}));
        QVERIFY(QFileInfo(QString::fromStdString(job.at("capture").get<std::string>())).isAbsolute());
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        QVERIFY(QDir(out).entryList(QDir::Files | QDir::Hidden).isEmpty());
        write(out + "/.keep", "preserve");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success"));
        QCOMPARE(read(out + "/.keep"), QByteArray("preserve"));
    }
    void invalidOptions_data() {
        QTest::addColumn<Json>("options");
        QTest::newRow("unknown") << Json{{"invented", 1}};
        QTest::newRow("boolean") << Json{{"x", true}};
        QTest::newRow("negative") << Json{{"y", -1}};
        QTest::newRow("overflow") << Json{{"sample", uint64_t(UINT32_MAX) + 1}};
        QTest::newRow("fraction") << Json{{"vertex", 1.5}};
        QTest::newRow("both-events") << Json{{"gpa_event", 1}, {"eid", 2}};
        QTest::newRow("short-group") << Json{{"group", {0, 0}}};
        QTest::newRow("thread-type") << Json{{"thread", "000"}};
        QTest::newRow("negative-thread") << Json{{"thread", {0, 0, -1}}};
        QTest::newRow("stage") << Json{{"stage", "VSIn"}};
        QTest::newRow("options-array") << Json::array();
    }
    void invalidOptions() {
        QFETCH(Json, options);
        QTemporaryDir dir;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, prepare(dir.path(), "success", options));
        QVERIFY(!QFileInfo::exists(dir.path() + "/output & 测试"));
    }
    void workerResult_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("message");
        QTest::newRow("success-surrogates") << "success" << "";
        QTest::newRow("missing-result") << "missing" << "did not produce a result";
        QTest::newRow("malformed-result") << "bad" << "Invalid analysis result";
        QTest::newRow("false-result") << "failed" << "fixture failure";
        QTest::newRow("numeric-success") << "numeric-ok" << "analysis failed";
        QTest::newRow("failed-exit") << "exit" << "worker failed";
    }
    void workerResult() {
        QFETCH(QString, mode);
        QFETCH(QString, message);
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), mode);
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        QString error;
        try {
            QCOMPARE(runRdcAnalysis(job, QCoreApplication::applicationFilePath(), 10), out + "/result.json");
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        if (message.isEmpty()) {
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(read(out + "/result.json"),
                     QByteArray("{\"ok\":true,\"annotation\":\"\\udc80\",\"value\":18446744073709551615}"));
        } else
            QVERIFY2(error.contains(message), qPrintable(error));
        QCOMPARE(Json::parse(read(out + "/job.json").toStdString()), job);
        QCOMPARE(read(out + "/worker.log"), QByteArray("mock stdout\nmock stderr"));
    }
    void timeoutKillsChildren() {
        QTemporaryDir dir;
        const auto job = prepare(dir.path(), "timeout");
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        QElapsedTimer timer;
        timer.start();
        QString error;
        try {
            runRdcAnalysis(job, QCoreApplication::applicationFilePath(), 0.75);
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        QVERIFY2(error.contains("timed out"), qPrintable(error));
        QVERIFY(timer.elapsed() < 5000);
        const auto pid = read(out + "/child-pid.txt").toUInt();
        const auto child = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (child) {
            const auto status = WaitForSingleObject(child, 2000);
            CloseHandle(child);
            QCOMPARE(status, DWORD(WAIT_OBJECT_0));
        }
        QVERIFY(!QFileInfo::exists(out + "/child-survived.txt"));
        QVERIFY(read(out + "/worker.log").contains("mock stdout"));
        QCOMPARE(Json::parse(read(out + "/job.json").toStdString()), job);
    }
    void invalidTimeoutAndStart() {
        QTemporaryDir dir;
        auto job = prepare(dir.path(), "success");
        const auto out = QString::fromStdString(job.at("out").get<std::string>());
        for (const auto seconds : {0., -1., std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN(), 1e20})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     runRdcAnalysis(job, QCoreApplication::applicationFilePath(), seconds));
        QVERIFY(QDir(out).entryList(QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, runRdcAnalysis(job, dir.path() + "/library.dll", 10));
        QVERIFY(QFileInfo::exists(out + "/worker.log"));
        QCOMPARE(Json::parse(read(out + "/job.json").toStdString()), job);
    }
    void publicCliArguments() {
        QTemporaryDir dir;
        const auto capture = dir.path() + "/capture & 测试.rdc";
        write(capture, "invalid capture: the library check must run first");
        const auto library = qEnvironmentVariable("WINDIR") + "/System32/version.dll";
        const auto out = dir.path() + "/CLI 结果";
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", {"rdc-analyze",
                                                                                     capture,
                                                                                     "inventory",
                                                                                     "--out",
                                                                                     out,
                                                                                     "--renderdoc",
                                                                                     library,
                                                                                     "--gpa-event",
                                                                                     "18446744073709551615",
                                                                                     "--resource",
                                                                                     "18446744073709551615",
                                                                                     "--sample",
                                                                                     "4294967295",
                                                                                     "--group",
                                                                                     "1",
                                                                                     "2",
                                                                                     "3",
                                                                                     "--thread",
                                                                                     "4",
                                                                                     "5",
                                                                                     "6"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains("Missing RenderDoc entry point"));
        const auto job = Json::parse(read(out + "/job.json").toStdString());
        QCOMPARE(job.at("gpa_event"), Json(UINT64_MAX));
        QCOMPARE(job.at("resource"), Json(UINT64_MAX));
        QCOMPARE(job.at("sample"), Json(UINT32_MAX));
        QCOMPARE(job.at("group"), Json::array({1, 2, 3}));
        QCOMPARE(job.at("thread"), Json::array({4, 5, 6}));
        QCOMPARE(job.at("index"), Json(nullptr));
        QVERIFY(QFileInfo::exists(out + "/worker.log"));
        const auto originalJob = read(out + "/job.json");
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"rdc-analyze", capture, "inventory", "--out", out, "--renderdoc", library});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(process.readAllStandardError().contains("new or empty"));
        QCOMPARE(read(out + "/job.json"), originalJob);
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe",
                      {"rdc-analyze", "--help"});
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains("FloraGPA.Cli rdc-analyze"));
    }
    void publicCliRejectsInvalidOptions_data() {
        QTest::addColumn<QStringList>("options");
        QTest::newRow("both-events") << QStringList{"--gpa-event", "1", "--eid", "2"};
        QTest::newRow("group-size") << QStringList{"--group", "0", "0"};
        QTest::newRow("thread-negative") << QStringList{"--thread", "0", "-1", "0"};
        QTest::newRow("overflow") << QStringList{"--mip", "4294967296"};
        QTest::newRow("negative-index") << QStringList{"--index", "-1"};
        QTest::newRow("fraction") << QStringList{"--x", "1.5"};
        QTest::newRow("stage") << QStringList{"--stage", "DSOut"};
        QTest::newRow("option") << QStringList{"--invented", "1"};
        QTest::newRow("timeout-zero") << QStringList{"--timeout", "0"};
        QTest::newRow("timeout-infinite") << QStringList{"--timeout", "inf"};
    }
    void publicCliRejectsInvalidOptions() {
        QFETCH(QStringList, options);
        QTemporaryDir dir;
        const auto capture = dir.path() + "/capture.rdc";
        write(capture, "unused");
        const auto out = dir.path() + "/output";
        QStringList args{"rdc-analyze",
                         capture,
                         "inventory",
                         "--out",
                         out,
                         "--renderdoc",
                         qEnvironmentVariable("WINDIR") + "/System32/version.dll"};
        args.append(options);
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(!process.readAllStandardError().isEmpty());
        QVERIFY(!QFileInfo::exists(out + "/result.json"));
        QVERIFY(!QFileInfo::exists(out + "/job.json"));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 3 && args[1] == "--job")
        return mockWorker(Json::parse(read(args[2]).toStdString()));
    if (args.size() == 3 && args[1] == "--delayed-child") {
        QThread::msleep(3000);
        write(args[2], "child survived");
        return 0;
    }
    RdcJobTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "RdcJobTests.moc"
