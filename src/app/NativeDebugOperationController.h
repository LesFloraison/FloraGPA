#pragma once
#include "application/NativeDebugSession.h"
#include <QFutureWatcher>
#include <QObject>
#include <atomic>

namespace flora {
struct NativeDebugOperationOutcome {
    nlohmann::json snapshot;
    QString error, exported;
    bool cancelled = false;
};
// Holds exclusive session access until finished. Completed operations win over
// cancellation delivered after their final check (or archive publication).
class NativeDebugOperationController final : public QObject {
    Q_OBJECT
  public:
    explicit NativeDebugOperationController(QObject *parent = nullptr);
    ~NativeDebugOperationController() override;
    bool active() const { return bool(cancelled_); }
    void cancel();
    void start(std::shared_ptr<NativeDebugSession> session, NativeDebugCommand command, size_t position,
               nlohmann::json configuration, std::string interpretation, const QString &archive = {},
               double timeoutSeconds = 180);
  signals:
    void finished(const flora::NativeDebugOperationOutcome &outcome);

  private:
    QFutureWatcher<NativeDebugOperationOutcome> watcher_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};
} // namespace flora
