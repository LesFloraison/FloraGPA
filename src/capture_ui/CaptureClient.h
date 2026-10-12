#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace flora::capture_ui {
// CLI-only composition client. No capture runtime or replay library dependency.
class CaptureClient final : public QObject {
    Q_OBJECT
  public:
    explicit CaptureClient(QObject *parent = nullptr);
    void setController(const QString &path) { controller_ = path; }
    QString sessionFile() const { return session_; }
    bool busy() const { return !command_.isEmpty(); }
    bool connected() const { return connected_; }
    struct HudSettings { QString mode="compact", modeKey="Ctrl+F1", captureKey="Ctrl+Shift+C"; };
    void launch(const QString &exe, const QString &cwd, const QString &out, const QStringList &arguments);
    void launch(const QString &exe, const QString &cwd, const QString &out, const QStringList &arguments, const HudSettings &hud);
    void status();
    void capture(const QString &swapChain = {});
    void selectSwap(const QString &swapChain);
    void cancel();
    void disconnectSession();
  signals:
    void busyChanged(bool busy);
    void response(const QString &command, const QJsonObject &result);
    void failed(const QString &message);
  private:
    void start(const QString &command, const QStringList &arguments);
    void sessionCommand(const QString &command, const QStringList &extra = {});
    void consume();
    void finish(int exitCode, QProcess::ExitStatus status);
    QProcess process_;
    QTimer deadline_;
    QByteArray output_;
    QString controller_, session_, command_, transportError_;
    bool connected_ = false;
};
}
