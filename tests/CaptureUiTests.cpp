#include "capture_ui/CaptureClient.h"
#include "capture_ui/CaptureDialog.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#define NOMINMAX
#include <Windows.h>
using namespace flora::capture_ui;
class CaptureUiTests final : public QObject {
    Q_OBJECT
    QString controller() { return QCoreApplication::applicationDirPath()+"/FloraCaptureMock.exe"; }
  private slots:
    void cleanup() { qunsetenv("FLORA_CAPTURE_MOCK"); }
    void unavailableActions() {
        CaptureDialog dialog(controller());
        QVERIFY(!dialog.findChild<QPushButton *>("captureNext")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton *>("captureOpen")->isEnabled());
    }
    void sessionLifecycle() {
        QTemporaryDir dir;QVERIFY(dir.isValid());CaptureClient client;client.setController(controller());
        QSignalSpy responses(&client,&CaptureClient::response),errors(&client,&CaptureClient::failed);
        client.launch("target.exe",{},dir.path(),{"argument with spaces"});
        QTRY_COMPARE(responses.size(),1);QVERIFY(client.connected());QVERIFY(!client.busy());
        client.capture();QTRY_COMPARE(responses.size(),2);
        QCOMPARE(responses.last()[1].toJsonObject().value("error").toString(),"capture_capability_incomplete");
        client.status();QTRY_COMPARE(responses.size(),3);
        client.disconnectSession();QTRY_COMPARE(responses.size(),4);QVERIFY(!client.connected());
        QCOMPARE(errors.size(),0);
    }
    void processFailures_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("malformed")<<QByteArray("malformed");
        QTest::newRow("oversized")<<QByteArray("oversized");
        QTest::newRow("contradiction")<<QByteArray("contradiction");
    }
    void processFailures() {
        QFETCH(QByteArray,mode);qputenv("FLORA_CAPTURE_MOCK",mode);
        QTemporaryDir dir;CaptureClient client;client.setController(controller());
        QSignalSpy responses(&client,&CaptureClient::response),errors(&client,&CaptureClient::failed);
        client.launch("target.exe",{},dir.path(),{});
        QTRY_COMPARE(errors.size(),1);QCOMPARE(responses.size(),0);QVERIFY(!client.connected());QVERIFY(!client.busy());
    }
    void concurrentRequest() {
        qputenv("FLORA_CAPTURE_MOCK","slow");QTemporaryDir dir;CaptureClient client;client.setController(controller());
        QSignalSpy errors(&client,&CaptureClient::failed),responses(&client,&CaptureClient::response);
        client.launch("target.exe",{},dir.path(),{});client.launch("another.exe",{},dir.path(),{});
        QCOMPARE(errors.size(),1);QTRY_COMPARE(responses.size(),1);QVERIFY(client.connected());
    }
    void targetExit() {
        QTemporaryDir dir;CaptureClient client;client.setController(controller());QSignalSpy responses(&client,&CaptureClient::response);
        client.launch("target.exe",{},dir.path(),{});QTRY_COMPARE(responses.size(),1);
        qputenv("FLORA_CAPTURE_MOCK","exited");client.status();QTRY_COMPARE(responses.size(),2);QVERIFY(!client.connected());
    }
    void nativeCaptureDialog() {
        const auto native=qEnvironmentVariable("FLORA_NATIVE_CAPTURE_CONTROLLER"), target=qEnvironmentVariable("FLORA_NATIVE_LIVE_TARGET");
        if(native.isEmpty() || target.isEmpty())QSKIP("Native capture dialog requires capture-full");
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto oracle=dir.path()+"/target";
        struct Cleanup {QString oracle;HANDLE process=nullptr;~Cleanup(){QFile done(oracle+"/finish.signal");if(done.open(QIODevice::WriteOnly))done.close();if(process){WaitForSingleObject(process,10000);CloseHandle(process);}}} cleanup{oracle};
        CaptureDialog dialog(native);auto* client=dialog.findChild<CaptureClient*>();QVERIFY(client);
        QSignalSpy responses(client,&CaptureClient::response),errors(client,&CaptureClient::failed),opened(&dialog,&CaptureDialog::openCaptureRequested);
        dialog.findChild<QLineEdit*>("captureExecutable")->setText(target);
        dialog.findChild<QLineEdit*>("captureOutput")->setText(dir.path()+"/session");
        dialog.findChild<QLineEdit*>("captureArguments")->setText("\""+oracle+"\" 0");
        dialog.findChild<QPushButton*>("captureLaunch")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!responses.empty(),20000);const auto launched=responses.first()[1].toJsonObject();QVERIFY(launched.value("ok").toBool());
        cleanup.process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(launched.value("pid").toInteger()));QVERIFY(cleanup.process);
        auto* capture=dialog.findChild<QPushButton*>("captureNext");auto* open=dialog.findChild<QPushButton*>("captureOpen");
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),20000);auto* swaps=dialog.findChild<QComboBox*>("captureSwapChains");QVERIFY(swaps->findData("1")>=0);swaps->setCurrentIndex(swaps->findData("1"));
        capture->click();QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(),20000);open->click();QCOMPARE(opened.size(),1);
        const auto path=opened.first()[0].toString();QVERIFY(QFileInfo(path).isAbsolute());QVERIFY(QFileInfo(path).isFile());QCOMPARE(QFileInfo(path).suffix(),"gpa_frame");
        QFile unsupported(oracle+"/update.signal");QVERIFY(unsupported.open(QIODevice::WriteOnly));unsupported.close();
        QTRY_VERIFY(capture->isEnabled());capture->click();auto* status=dialog.findChild<QLabel*>("captureStatus");QTRY_COMPARE_WITH_TIMEOUT(status->text(),"Capture failed",20000);
        QVERIFY(status->toolTip().contains("UpdateSubresource"));QVERIFY(open->isEnabled());open->click();QCOMPARE(opened.last()[0].toString(),path);
        dialog.findChild<QPushButton*>("captureDisconnect")->click();QTRY_VERIFY(!client->connected());QCOMPARE(WaitForSingleObject(cleanup.process,0),DWORD(WAIT_TIMEOUT));QCOMPARE(errors.size(),0);
    }
    void nativeLifecycle() {
        const auto native=qEnvironmentVariable("FLORA_NATIVE_CAPTURE_CONTROLLER");
        if(native.isEmpty()) QSKIP("Native bootstrap is tested by the capture-full profile");
        const auto probe=qEnvironmentVariable("FLORA_NATIVE_CAPTURE_PROBE");
        QTemporaryDir dir;QVERIFY(dir.isValid());CaptureClient client;client.setController(native);
        QSignalSpy responses(&client,&CaptureClient::response),errors(&client,&CaptureClient::failed);
        client.launch(probe,{},dir.path()+"/session",{dir.path()+"/oracle","1"});
        QTRY_COMPARE_WITH_TIMEOUT(responses.size(),1,20000);
        const auto launch=responses.last()[1].toJsonObject();QVERIFY(launch.value("ok").toBool());QVERIFY(client.connected());
        const auto pid=DWORD(launch.value("pid").toInteger());
        HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);QVERIFY(process!=nullptr);
        struct Close {HANDLE process;~Close(){CloseHandle(process);}} close{process};
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.path()+"/oracle/ready.json"),15000);
        client.status();QTRY_COMPARE(responses.size(),2);
        const auto status=responses.last()[1].toJsonObject();QVERIFY(status.value("ok").toBool());
        QVERIFY(!status.value("capture_available").toBool());
        client.capture("1");QTRY_COMPARE(responses.size(),3);
        QCOMPARE(responses.last()[1].toJsonObject().value("error").toString(),"capture_capability_incomplete");
        client.cancel();QTRY_COMPARE(responses.size(),4);
        QVERIFY(!responses.last()[1].toJsonObject().value("cancelled").toBool());
        client.disconnectSession();QTRY_COMPARE(responses.size(),5);QVERIFY(!client.connected());
        QCOMPARE(WaitForSingleObject(process,0),DWORD(WAIT_TIMEOUT));
        QFile signal(dir.path()+"/oracle/finish.signal");QVERIFY(signal.open(QIODevice::WriteOnly));signal.close();
        QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(process,0),DWORD(WAIT_OBJECT_0),10000);
        QCOMPARE(errors.size(),0);
    }
};
QTEST_MAIN(CaptureUiTests)
#include "CaptureUiTests.moc"
