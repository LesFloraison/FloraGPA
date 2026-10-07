#include "WorkerReport.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>

namespace flora {
WorkerReport readWorkerReport(const QString &directory, bool replay, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    QFile file(QDir(directory).filePath("report.json"));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Worker report is missing or unreadable");
    const auto length = file.size();
    QByteArray bytes;
    while (bytes.size() < length) {
        checkCancellation(cancelled);
        const auto chunk = file.read(std::min<qint64>(1024 * 1024, length - bytes.size()));
        if (chunk.isEmpty() || file.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot read complete worker report");
        bytes.append(chunk);
    }
    checkCancellation(cancelled);
    if (file.pos() != length || file.size() != length || file.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read complete worker report");
    // Qt parsing is not interruptible within one call. It runs off the UI
    // thread, bounded by this request's lifetime; cancellation wins afterwards.
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    checkCancellation(cancelled);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Worker report is invalid JSON or not an object");
    WorkerReport result{document.object(), {}};
    if (!result.report["completed"].isBool() || !result.report["completed"].toBool())
        throw std::runtime_error("Worker did not complete");
    if (replay && result.report["image_available"].toBool(true)) {
        size_t events = 0;
        result.replayReport = nlohmann::json::parse(bytes.begin(), bytes.end(),
            [&](int, nlohmann::json::parse_event_t, nlohmann::json &) {
                if (++events % 256 == 0) checkCancellation(cancelled);
                return true;
            });
    }
    checkCancellation(cancelled);
    return result;
}
} // namespace flora
