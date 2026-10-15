#include "CaptureClient.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QFile>

namespace flora::capture_ui {
CaptureClient::CaptureClient(QObject *parent) : QObject(parent) {
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        transportError_ = "Capture controller timed out"; process_.kill();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &CaptureClient::consume);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { process_.readAllStandardError(); });
    connect(&process_, &QProcess::finished, this, &CaptureClient::finish);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            deadline_.stop(); command_.clear(); emit busyChanged(false); emit failed(process_.errorString());
        }
    });
}
void CaptureClient::consume() {
    const auto bytes = process_.readAllStandardOutput();
    if (output_.size() + bytes.size() > 1024 * 1024) {
        transportError_ = "Capture controller response is too large"; process_.kill();
    } else output_ += bytes;
}
void CaptureClient::start(const QString &command, const QStringList &arguments) {
    if (busy()) { emit failed("Capture controller is busy"); return; }
    if (!QFileInfo(controller_).isFile()) { emit failed("Capture controller was not found"); return; }
    command_ = command; output_.clear(); transportError_.clear();
    process_.setProgram(controller_); process_.setArguments(QStringList{command} + arguments);
    emit busyChanged(true); process_.start(); deadline_.start(25000);
}
void CaptureClient::finish(int exitCode, QProcess::ExitStatus exitStatus) {
    if (command_.isEmpty()) return;
    consume(); deadline_.stop(); const auto command = command_; command_.clear();
    emit busyChanged(false);
    if (!transportError_.isEmpty()) { emit failed(transportError_); return; }
    QJsonParseError parse{}; const auto doc = QJsonDocument::fromJson(output_, &parse);
    if (exitStatus != QProcess::NormalExit || parse.error != QJsonParseError::NoError || !doc.isObject() ||
        !doc.object().value("ok").isBool()) { emit failed("Invalid capture controller response"); return; }
    const auto result = doc.object();
    if (result.value("ok").toBool() != (exitCode == 0)) { emit failed("Capture controller result disagrees with its exit code"); return; }
    if (command == "launch" && result.value("ok").toBool()) {
        const auto file = result.value("session_file").toString();
        if (result.value("version").toInt() != 4 || file.isEmpty() || !QFileInfo(file).isFile()) {
            emit failed("Invalid capture session manifest"); return;
        }
        session_ = file; connected_ = true;
    }
    if ((command == "disconnect" && result.value("ok").toBool()) || result.value("state").toString() == "Exited")
        connected_ = false;
    emit response(command, result);
}
void CaptureClient::launch(const QString &exe, const QString &cwd, const QString &out, const QStringList &arguments) {
    launch(exe,cwd,out,arguments,HudSettings{});
}
void CaptureClient::launch(const QString &exe, const QString &cwd, const QString &out, const QStringList &arguments, const HudSettings &hud) {
    if (connected_) { emit failed("Disconnect the current session before launching another target"); return; }
    QStringList args{"--exe",exe,"--out",out,"--hud",hud.mode,"--hud-hotkey",hud.modeKey,"--capture-hotkey",hud.captureKey};
    if (!cwd.isEmpty()) args << "--cwd" << cwd;
    if (!arguments.isEmpty()) args << "--" << arguments;
    start("launch",args);
}
void CaptureClient::sessionCommand(const QString &command, const QStringList &extra) {
    if (!connected_) { emit failed("No active capture session"); return; }
    start(command,QStringList{"--session",session_} + extra);
}
void CaptureClient::status() { sessionCommand("status"); }
void CaptureClient::capture(const QString &swapChain) {
    sessionCommand("capture", swapChain.isEmpty() ? QStringList{} : QStringList{"--swap-chain",swapChain});
}
void CaptureClient::cancel() { sessionCommand("cancel"); }
void CaptureClient::selectSwap(const QString &swapChain) { sessionCommand("select",{"--swap-chain",swapChain.isEmpty()?QString("0"):swapChain}); }
void CaptureClient::disconnectSession() { sessionCommand("disconnect"); }
void CaptureClient::useSession(const QString& manifest) {
    if(busy()){emit failed("Capture controller is busy");return;}
    QFile file(manifest);if(!QFileInfo(file).isAbsolute() || !file.open(QIODevice::ReadOnly) || file.size()>65536){emit failed("Invalid capture session manifest");return;}
    const auto value=QJsonDocument::fromJson(file.readAll()).object();
    if(value.value("version").toInt()!=4 || value.value("pid").toInteger()<=0 || value.value("process_created").toInteger()<=0 || value.value("session_id").toString().isEmpty()){
        emit failed("Invalid capture session manifest");return;
    }
    session_=manifest;connected_=true;status();
}
}
