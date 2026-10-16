#include "capture_ui/CaptureClient.h"
#include "capture_ui/CaptureDialog.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QAbstractItemView>
#include <QLabel>
#include <QCheckBox>
#include <QJsonArray>
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
    void autoDetectLifecycle() {
        QTemporaryDir root;CaptureDialog dialog(controller());
        auto* check=dialog.findChild<QCheckBox*>("captureAutoDetect");
        auto* sessions=dialog.findChild<QComboBox*>("captureSessions");
        auto* state=dialog.findChild<QLabel*>("captureAutoDetectStatus");
        QVERIFY(check && sessions && state);QVERIFY(!check->isChecked());
        dialog.findChild<QLineEdit*>("captureOutput")->setText(root.path());
        dialog.show();check->setChecked(true);
        QTRY_COMPARE(state->text(),"Watching");QTRY_COMPARE(sessions->count(),2);
        QVERIFY(dialog.findChild<QLineEdit*>("captureExecutable")->text().isEmpty());
        dialog.close();QVERIFY(!dialog.isVisible());QCOMPARE(state->text(),"Watching");
        QTRY_VERIFY(sessions->isEnabled());sessions->setCurrentIndex(1);emit sessions->activated(1);
        QTRY_VERIFY(dialog.findChild<QPushButton*>("captureDisconnect")->isEnabled());
        check->setChecked(false);QTRY_COMPARE(state->text(),"Off");
        QVERIFY(dialog.findChild<QPushButton*>("captureDisconnect")->isEnabled());
    }
    void pollingPreservesPopup() {
        QTemporaryDir root;CaptureDialog dialog(controller());dialog.show();
        auto* client=dialog.findChild<CaptureClient*>();QSignalSpy responses(client,&CaptureClient::response);
        client->launch("target.exe",{},root.path(),{});QTRY_VERIFY(client->connected() && !client->busy());
        for(const auto* name:{"captureSwapChains","captureResourceLimit"}){
            auto* combo=dialog.findChild<QComboBox*>(name);combo->showPopup();QVERIFY(combo->view()->isVisible());
            const auto count=responses.size();QTest::qWait(1500);
            QVERIFY(combo->view()->isVisible());QCOMPARE(responses.size(),count);
            combo->hidePopup();QTRY_VERIFY(responses.size()>count);QTRY_VERIFY(!client->busy());
        }
    }
    void autoDetectFailure() {
        qputenv("FLORA_CAPTURE_MOCK","monitor-denied");AutoDetectClient client;QTemporaryDir root;
        QSignalSpy failures(&client,&AutoDetectClient::failed);client.start(controller(),root.path(),{});
        QTRY_COMPARE(failures.size(),1);QCOMPARE(failures.first().first().toString(),"Elevation denied");QTRY_VERIFY(!client.active());
    }
    void autoDetectProgress() {
        qputenv("FLORA_CAPTURE_MOCK","monitor-progress");AutoDetectClient client;QTemporaryDir root;
        QSignalSpy progress(&client,&AutoDetectClient::attachmentProgress),failures(&client,&AutoDetectClient::failed);
        const auto target=root.path()+"/Game With Spaces.exe";
        client.start(controller(),root.path(),{},target);
        QTRY_COMPARE(progress.size(),4);QCOMPARE(failures.size(),0);
        QCOMPARE(progress.last()[0].toJsonObject().value("state").toString(),"Attach failed");
        const auto json=progress.first()[0].toJsonObject().value("arguments").toArray();QStringList args;for(const auto& arg:json)args<<arg.toString();
        QCOMPARE(args.value(args.indexOf("--target-exe")+1),target);
        QVERIFY(client.active());client.stop();QTRY_VERIFY(!client.active());
    }
    void autoDetectPrefersTarget() {
        QTemporaryDir root;CaptureDialog dialog(controller());
        dialog.findChild<QLineEdit*>("captureOutput")->setText(root.path());
        dialog.findChild<QLineEdit*>("captureExecutable")->setText(root.path()+"/Endfield.exe");
        auto* client=dialog.findChild<CaptureClient*>();
        auto* sessions=dialog.findChild<QComboBox*>("captureSessions");
        dialog.findChild<QCheckBox*>("captureAutoDetect")->setChecked(true);
        QTRY_COMPARE(sessions->count(),2);QTRY_VERIFY(client->connected());
        QVERIFY(client->sessionFile().endsWith("session2.json"));
        dialog.findChild<QCheckBox*>("captureAutoDetect")->setChecked(false);
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
    void hudOptions() {
        QTemporaryDir dir;CaptureClient client;client.setController(controller());QSignalSpy responses(&client,&CaptureClient::response);
        client.launch("target.exe",{},dir.path(),{"target argument"},{"off","Alt+F3","off"});QTRY_COMPARE(responses.size(),1);
        const auto json=responses.first()[1].toJsonObject().value("arguments").toArray();QStringList args;for(const auto& arg:json)args<<arg.toString();
        QCOMPARE(args.value(args.indexOf("--hud")+1),"off");QCOMPARE(args.value(args.indexOf("--hud-hotkey")+1),"Alt+F3");QCOMPARE(args.value(args.indexOf("--capture-hotkey")+1),"off");
        QVERIFY(args.indexOf("--hud")<args.indexOf("--"));QCOMPARE(args.last(),"target argument");
        CaptureDialog dialog(controller());QCOMPARE(dialog.findChild<QComboBox*>("captureHudMode")->currentData().toString(),"compact");
        QCOMPARE(dialog.findChild<QLineEdit*>("captureHudHotkey")->text(),"Ctrl+F1");QCOMPARE(dialog.findChild<QLineEdit*>("captureFrameHotkey")->text(),"Ctrl+Shift+C");
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
        QTRY_VERIFY_WITH_TIMEOUT(!responses.empty() || !errors.empty(),20000);
        QVERIFY2(!responses.empty(),qPrintable(errors.isEmpty()?QString("No launch response"):errors.first().first().toString()));
        const auto launched=responses.first()[1].toJsonObject();QVERIFY(launched.value("ok").toBool());
        cleanup.process=OpenProcess(SYNCHRONIZE,FALSE,DWORD(launched.value("pid").toInteger()));QVERIFY(cleanup.process);
        auto* capture=dialog.findChild<QPushButton*>("captureNext");auto* open=dialog.findChild<QPushButton*>("captureOpen");
        auto* limit=dialog.findChild<QComboBox*>("captureResourceLimit");QVERIFY(limit);QCOMPARE(limit->currentData().toUInt(),1024u);limit->setCurrentIndex(limit->findData(4096u));
        QTRY_VERIFY_WITH_TIMEOUT(capture->isEnabled(),20000);auto* swaps=dialog.findChild<QComboBox*>("captureSwapChains");QVERIFY(swaps->findData("1")>=0);swaps->setCurrentIndex(swaps->findData("1"));
        capture->click();QTRY_VERIFY_WITH_TIMEOUT(open->isEnabled(),20000);open->click();QCOMPARE(opened.size(),1);
        bool budgetSeen=false;for(const auto& response:responses){const auto result=response[1].toJsonObject();if(result.value("last_capture").toObject().value("resource_limit_mib").toInt()==4096)budgetSeen=true;}QVERIFY(budgetSeen);
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
