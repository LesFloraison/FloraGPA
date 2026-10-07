#pragma once
#include "core/Frame.h"
#include <QJsonObject>
#include <QString>
#include <nlohmann/json.hpp>

namespace flora {
struct WorkerReport {
    QJsonObject report;
    nlohmann::json replayReport;
    nlohmann::json payload;
    QJsonObject geometry;
};
// Common report only; analyzer-specific payloads have separate readers.
WorkerReport readWorkerReport(const QString &directory, bool replay,
                              const CancelCheck &cancelled = {});
// Fixed analyzer filenames only; publication remains on the owning UI thread.
WorkerReport readWorkerOutput(const QString &directory, const QString &kind,
                              const CancelCheck &cancelled = {});
} // namespace flora
