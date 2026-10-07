#pragma once
#include "core/Frame.h"
#include <QJsonObject>
#include <QString>
#include <nlohmann/json.hpp>

namespace flora {
struct WorkerReport {
    QJsonObject report;
    nlohmann::json replayReport;
};
// Common report only; analyzer-specific payloads have separate readers.
WorkerReport readWorkerReport(const QString &directory, bool replay,
                              const CancelCheck &cancelled = {});
} // namespace flora
