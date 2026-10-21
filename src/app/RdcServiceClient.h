#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <nlohmann/json.hpp>
#include <functional>

namespace flora {
// One in-flight request. Navigation invalidates its result without unloading the capture.
class RdcServiceClient final : public QObject {
    QProcess process_;
    QTimer timeout_;
    QByteArray input_, diagnostics_;
    nlohmann::json job_;
    QString key_, executable_;
    uint64_t serial_{}, generation_{}, requestId_{};
    bool busy_{}, stopping_{}, retrying_{};
    unsigned retries_{};
    void *processJob_{};
    std::function<void(int, QString, bool)> completed_;
    void send(const char *command, const nlohmann::json &extra = {});
    void start();
    void dispatch();
    void read();
    void failed(const QString &error);
    void stopProcess();
  public:
    explicit RdcServiceClient(QObject *parent = nullptr);
    ~RdcServiceClient() override;
    bool busy() const { return busy_; }
    bool running() const { return process_.state() != QProcess::NotRunning; }
    void submit(const QString &executable, const QString &key, const nlohmann::json &job,
                std::function<void(int, QString, bool)> completed);
    void cancelSelection();
    void close();
};
}
