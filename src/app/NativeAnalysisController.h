#pragma once
#include "application/NativeArtifacts.h"
#include <QFutureWatcher>
#include <QObject>
#include <atomic>
#include <memory>

namespace flora {
class NativeDebugSession;
struct NativeReplayMesh;
struct NativeAnalysisOutcome {
    nlohmann::json job, result;
    NativeArtifactBytes artifacts;
    std::shared_ptr<NativeDebugSession> debugSession;
    std::shared_ptr<NativeReplayMesh> mesh;
    QString error;
    QByteArray workerLog;
    bool cancelled = false;
};
// Owns the complete hash/Worker/verification operation without blocking the UI thread.
class NativeAnalysisController final : public QObject {
    Q_OBJECT
  public:
    explicit NativeAnalysisController(QObject *parent = nullptr);
    ~NativeAnalysisController() override;
    bool active() const { return bool(cancelled_); }
    void start(const QString &capture, const QString &worker, const std::string &action,
               const nlohmann::json &options, const nlohmann::json &experiment, double timeoutSeconds = 180,
               uint64_t maximumBytes = 256ull * 1024 * 1024);
    void cancel();
    // Revalidate a saved job and its payloads using the same asynchronous loader.
    void loadDebugSession(nlohmann::json job, nlohmann::json result, NativeArtifactBytes artifacts,
                          double timeoutSeconds = 180, uint64_t maximumBytes = 256ull * 1024 * 1024);
  signals:
    void finished(const flora::NativeAnalysisOutcome &outcome);

  private:
    QFutureWatcher<NativeAnalysisOutcome> watcher_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};
} // namespace flora
