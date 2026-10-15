#pragma once
#include "CaptureClient.h"
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

namespace flora::capture_ui {
class AutoDetectClient final : public QObject {
    Q_OBJECT
public:
    explicit AutoDetectClient(QObject* parent=nullptr);
    ~AutoDetectClient() override;
    bool active() const { return active_; }
    void start(const QString& controller,const QString& output,const CaptureClient::HudSettings& hud);
    void stop();
    void recover(const QString& controller);
signals:
    void stateChanged(const QString& state);
    void sessionFound(const QJsonObject& manifest);
    void failed(const QString& error);
private:
    void consume();
    QProcess process_;
    QTimer deadline_;
    QByteArray buffer_;
    bool active_=false,stopped_=false,reportedError_=false;
};
}
