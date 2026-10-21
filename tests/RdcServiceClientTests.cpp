#include "app/RdcServiceClient.h"
#include <QCoreApplication>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <iostream>
#define NOMINMAX
#include <Windows.h>
using namespace flora;
using Json = nlohmann::json;
namespace {
void trace(const char *event) {
    QFile f(qEnvironmentVariable("FLORA_SERVICE_TEST_TRACE"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Append))
        f.write(QByteArray(event) + ' ' + QByteArray::number(GetCurrentProcessId()) + '\n');
}
QByteArray read(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }
int serve() {
    trace("start");
    const auto mode = qEnvironmentVariable("FLORA_SERVICE_TEST_MODE");
    for (std::string line; std::getline(std::cin, line);) {
        auto message = Json::parse(line);
        const auto command = message.at("command").get<std::string>();
        trace(command.c_str());
        if (mode == "crash") return 17;
        if (mode == "hang" && command == "execute") { QThread::sleep(30); continue; }
        Json response{{"schema", 1}, {"id", message.at("id")}, {"generation", message.at("generation")},
                      {"command", command}, {"ok", mode != "open-failure" || command != "open"}};
        if (mode == "wrong-generation") response["generation"] = "999999";
        if (!response.at("ok").get<bool>()) response["error"] = "Controller allocation failed";
        std::cout << response.dump() << '\n' << std::flush;
    }
    return 0;
}
}
class RdcServiceClientTests final : public QObject {
    Q_OBJECT
  private slots:
    void lifecycle_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"success", "crash", "wrong-generation", "open-failure", "hang"})
            QTest::newRow(mode) << QString(mode);
    }
    void lifecycle() {
        QFETCH(QString, mode);
        QTemporaryDir dir;
        const auto path = dir.filePath("trace.txt");
        qputenv("FLORA_SERVICE_TEST_MODE", mode.toUtf8());
        qputenv("FLORA_SERVICE_TEST_TRACE", path.toUtf8());
        auto restore = qScopeGuard([] { qunsetenv("FLORA_SERVICE_TEST_MODE"); qunsetenv("FLORA_SERVICE_TEST_TRACE"); });
        RdcServiceClient client;
        int completed{}, code{}; bool fallback{};
        auto finish = [&](int result, QString, bool legacy) { ++completed; code = result; fallback = legacy; };
        Json job{{"out", dir.filePath("result").toStdString()}};
        client.submit(QCoreApplication::applicationFilePath(), "capture-A", job, finish);
        if (mode == "hang") {
            QTRY_VERIFY_WITH_TIMEOUT(read(path).contains("execute"), 10000);
            const auto pid = read(path).split('\n').front().split(' ').last().toULong();
            const auto handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            QVERIFY(handle);
            auto release = qScopeGuard([&] { CloseHandle(handle); });
            client.cancelSelection();
            QVERIFY(client.running()); // Ordinary navigation does not kill the controller.
            client.close();
            QCOMPARE(WaitForSingleObject(handle, 3000), DWORD(WAIT_OBJECT_0));
            QCOMPARE(completed, 0);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 10000);
            if (mode == "success") {
                QCOMPARE(code, 0); QVERIFY(!fallback); QVERIFY(client.running());
                client.submit(QCoreApplication::applicationFilePath(), "capture-A", job, finish);
                QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 10000);
                QCOMPARE(read(path).count("start "), 1);
                QCOMPARE(read(path).count("open "), 1);
                QCOMPARE(read(path).count("execute "), 2);
            } else {
                QCOMPARE(code, 1);
                QCOMPARE(fallback, mode != "open-failure");
                QCOMPARE(read(path).count("start "), mode == "open-failure" ? 1 : 2);
                QVERIFY(!client.running());
            }
            client.close();
        }
        QVERIFY(!client.busy()); QVERIFY(!client.running());
        QTest::qWait(50);
        QCOMPARE(completed, mode == "hang" ? 0 : mode == "success" ? 2 : 1);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--serve")) return serve();
    RdcServiceClientTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "RdcServiceClientTests.moc"
