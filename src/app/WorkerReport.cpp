#include "WorkerReport.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

namespace flora {
namespace {
QByteArray readBytes(const QString &path, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Worker output is missing or unreadable: " + QFileInfo(path).fileName()).toStdString());
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
    return bytes;
}
QJsonObject qtObject(const QByteArray &bytes, const CancelCheck &cancelled) {
    // Qt parsing is not interruptible within one call. It runs off the UI
    // thread, bounded by this request's lifetime; cancellation wins afterwards.
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    checkCancellation(cancelled);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Worker report is invalid JSON or not an object");
    return document.object();
}
nlohmann::json nativeObject(const QByteArray &bytes, const CancelCheck &cancelled) {
    size_t events = 0;
    auto result = nlohmann::json::parse(bytes.begin(), bytes.end(),
        [&](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
            if (depth > 1024) throw std::runtime_error("Worker output nesting limit exceeded");
            if (++events % 256 == 0) checkCancellation(cancelled);
            return true;
        });
    checkCancellation(cancelled);
    if (!result.is_object()) throw std::runtime_error("Worker output is not an object");
    return result;
}
}
WorkerReport readWorkerReport(const QString &directory, bool replay, const CancelCheck &cancelled) {
    const auto bytes = readBytes(QDir(directory).filePath("report.json"), cancelled);
    WorkerReport result;
    result.report = qtObject(bytes, cancelled);
    if (!result.report["completed"].isBool() || !result.report["completed"].toBool())
        throw std::runtime_error("Worker did not complete");
    if (replay && result.report["image_available"].toBool(true)) {
        result.replayReport = nativeObject(bytes, cancelled);
    }
    checkCancellation(cancelled);
    return result;
}
WorkerReport readWorkerOutput(const QString &directory, const QString &kind, const CancelCheck &cancelled) {
    auto result = readWorkerReport(directory, kind == "replay", cancelled);
    QString filename;
    if (kind == "quad") filename = "quad.json";
    else if (kind == "coverage") filename = "coverage.json";
    else if (kind == "timings") filename = "profile.json";
    else if (kind == "statistics") filename = "statistics.json";
    else if (kind == "replay-pipeline") filename = "replay-pipeline.json";
    else if (kind == "predicate") filename = "predicate.json";
    else if (kind == "geometry") filename = "geometry.json";
    else if (kind == "post-geometry") filename = "geometry-ui.json";
    if (!filename.isEmpty()) {
        try {
            const auto bytes = readBytes(QDir(directory).filePath(filename), cancelled);
            if (kind == "geometry" || kind == "post-geometry") result.geometry = qtObject(bytes, cancelled);
            else result.payload = nativeObject(bytes, cancelled);
        } catch (const OperationCancelled &) {
            throw;
        } catch (const std::exception &error) {
            throw std::runtime_error(("Worker " + filename + ": " + QString::fromUtf8(error.what())).toStdString());
        }
    }
    checkCancellation(cancelled);
    return result;
}
} // namespace flora
