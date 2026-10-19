#pragma once
#include "core/Frame.h"
#include "DiagnosticOutput.h"
#include "ThumbnailOutput.h"
#include <QJsonObject>
#include <QString>
#include <nlohmann/json.hpp>
#include <optional>

namespace flora {
// Null project means no experiment was sent with this DX12 replay request.
void requireDx12ExperimentReport(const nlohmann::json &report, const nlohmann::json &project);
void requireDx12QueryPolicyReport(const nlohmann::json &report, bool isolateMissingQueries);
struct WorkerReport {
    QJsonObject report;
    nlohmann::json replayReport;
    nlohmann::json payload;
    QJsonObject geometry;
    std::optional<CoverageOutput> coverage;
    std::optional<QuadOutput> quad;
    std::optional<ThumbnailOutput> thumbnails;
};
// Common report only; analyzer-specific payloads have separate readers.
WorkerReport readWorkerReport(const QString &directory, bool replay,
                              const CancelCheck &cancelled = {});
// Fixed analyzer filenames only; publication remains on the owning UI thread.
WorkerReport readWorkerOutput(const QString &directory, const QString &kind,
                              const CancelCheck &cancelled = {}, const QString &diagnosticKey = {});
} // namespace flora
