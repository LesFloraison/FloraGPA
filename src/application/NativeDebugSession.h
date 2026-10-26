#pragma once
#include "NativeArtifacts.h"
#include "NativeExecutionDebugModel.h"
#include <QTemporaryDir>

namespace flora {
// A verified, privately retained compute analysis. Ownership may be transferred
// after loading; model access is exclusive and must not occur concurrently.
class NativeDebugSession final {
  public:
    using Json = nlohmann::json;
    NativeDebugSession(Json job, Json result, const NativeArtifactBytes &artifacts,
                       uint64_t maximumBytes = 256ull * 1024 * 1024, const CancelCheck &cancelled = {});
    const Json &job() const { return job_; }
    const Json &result() const { return result_; }
    NativeExecutionDebugModel &model() { return *model_; }
    QString tracePath() const { return directory_.filePath("trace.jsonl"); }
    const QByteArray &shader() const { return shader_; }
    const Json &initialSnapshot() const { return initialSnapshot_; }
    void exportArchive(const QString &path, const Json &state, const CancelCheck &cancelled = {}) const;

  private:
    Json job_, result_;
    Json initialSnapshot_;
    // Declaration order ensures the reader closes its file before cleanup.
    QTemporaryDir directory_;
    QByteArray shader_, metadata_;
    std::unique_ptr<NativeExecutionDebugModel> model_;
};
} // namespace flora
