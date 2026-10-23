#pragma once
#include "NativeAnalysis.h"
namespace flora {
nlohmann::json prepareNativeAnalysisJob(const QString &capture, const std::string &action,
                                        const QString &output, const QString &worker,
                                        const nlohmann::json &options = nlohmann::json::object(),
                                        const QString &experiment = {}, const CancelCheck &cancelled = {});
QString runNativeAnalysisJob(const nlohmann::json &job, const QString &worker, double timeoutSeconds,
                             const CancelCheck &cancelled = {});
int nativeAnalysisWorker(const QString &jobPath, const QString &readyPath);
} // namespace flora
