#include "WorkerReport.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QImageReader>
#include <limits>

namespace flora {
namespace {
QByteArray readBytes(const QString &path, const CancelCheck &cancelled,
                     std::optional<uint64_t> expected = {}) {
    checkCancellation(cancelled);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Worker output is missing or unreadable: " + QFileInfo(path).fileName()).toStdString());
    const auto length = file.size();
    if (length < 0 || (expected && uint64_t(length) != *expected))
        throw std::runtime_error(("Worker output length mismatch: " + QFileInfo(path).fileName()).toStdString());
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
uint64_t unsignedValue(const nlohmann::json &value, const char *field = "Diagnostic size") {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer() && value.get<int64_t>() >= 0) return uint64_t(value.get<int64_t>());
    throw std::runtime_error(std::string(field) + " is not an unsigned integer");
}
int dimension(const nlohmann::json &value) {
    const auto n = unsignedValue(value);
    if (!n || n > uint64_t(std::numeric_limits<int>::max() / 4))
        throw std::runtime_error("Diagnostic image dimension is invalid");
    return int(n);
}
QSize imageSize(const nlohmann::json &object, const char *width = "width", const char *height = "height") {
    return {dimension(object.at(width)), dimension(object.at(height))};
}
QImage decodeImage(const QByteArray &bytes, const QSize &size, const char *name,
                    const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read diagnostic image bytes");
    QImageReader reader(&buffer, "png");
    if (reader.size() != size)
        throw std::runtime_error(std::string(name) + ": PNG dimensions do not match the report");
    checkCancellation(cancelled);
    auto image = reader.read();
    checkCancellation(cancelled);
    if (image.isNull() || image.size() != size)
        throw std::runtime_error(std::string(name) + ": PNG is incomplete or unreadable");
    return image;
}
PreparedImage opaqueDiagnostic(QImage original, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    auto opaque = original.convertToFormat(QImage::Format_RGBA8888);
    checkCancellation(cancelled);
    if (opaque.isNull()) throw std::runtime_error("Cannot allocate diagnostic display conversion");
    for (int y = 0; y < opaque.height(); ++y) {
        checkCancellation(cancelled);
        auto row = opaque.scanLine(y);
        for (int x = 0; x < opaque.width(); ++x) row[x * 4 + 3] = 255;
    }
    auto result = prepareImageForDisplay(std::move(opaque), cancelled);
    result.original = std::move(original);
    return result;
}
QImage coverageTint(const QImage &mask, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    auto tint = mask.convertToFormat(QImage::Format_RGBA8888);
    checkCancellation(cancelled);
    if (tint.isNull()) throw std::runtime_error("Cannot allocate coverage mask conversion");
    for (int y = 0; y < tint.height(); ++y) {
        checkCancellation(cancelled);
        auto row = tint.scanLine(y);
        for (int x = 0; x < tint.width(); ++x) {
            const bool covered = row[x * 4] != 0;
            row[x * 4] = 255; row[x * 4 + 1] = 0; row[x * 4 + 2] = 255;
            row[x * 4 + 3] = covered ? 255 : 0;
        }
    }
    return prepareImageForDisplay(std::move(tint), cancelled).display;
}
}
CoverageOutput readCoverageOutput(const QString &directory, const CancelCheck &cancelled) {
    CoverageOutput result;
    const QDir root(directory);
    result.files[0] = readBytes(root.filePath(coverageOutputFiles[0]), cancelled);
    result.report = nativeObject(result.files[0], cancelled);
    const auto kind = result.report.at("target_kind").get<std::string>();
    const auto &shape = kind == "viewport" ? result.report :
                        kind == "buffer" ? result.report.at("target_buffer_view") :
                        result.report.at("target_subresource");
    if (kind != "viewport" && kind != "buffer" && kind != "color" && kind != "depth")
        throw std::runtime_error("Unknown coverage target kind");
    const auto size = imageSize(shape);
    for (size_t i = 1; i < result.files.size(); ++i) {
        result.files[i] = readBytes(root.filePath(coverageOutputFiles[i]), cancelled);
        auto image = decodeImage(result.files[i], size, coverageOutputFiles[i], cancelled);
        if (i == 1) result.mask = std::move(image);
        if (i == 3) result.diagnostic = opaqueDiagnostic(std::move(image), cancelled);
    }
    result.maskDisplay = coverageTint(result.mask, cancelled);
    checkCancellation(cancelled);
    return result;
}
QuadOutput readQuadOutput(const QString &directory, const QString &contextKey,
                          const CancelCheck &cancelled) {
    QuadOutput result;
    const QDir root(directory);
    result.report = nativeObject(readBytes(root.filePath("quad.json"), cancelled), cancelled);
    const auto size = imageSize(result.report, "quad_width", "quad_height");
    const auto sourceSize = imageSize(result.report);
    if (size.width() != (sourceSize.width() + 1) / 2 || size.height() != (sourceSize.height() + 1) / 2)
        throw std::runtime_error("Quad grid dimensions do not match source dimensions");
    const auto capacity = unsignedValue(result.report.at("histogram_capacity"));
    if (!capacity || capacity > uint64_t(std::numeric_limits<qint64>::max()) / 4)
        throw std::runtime_error("Quad histogram byte length overflows");
    const auto cells = uint64_t(size.width()) * uint64_t(size.height()) * 4;
    const std::array<uint64_t, 5> lengths{cells, cells, cells, capacity * 4, 16};
    for (size_t i = 0; i < lengths.size(); ++i)
        result.files[i] = readBytes(root.filePath(quadOutputFiles[i]), cancelled, lengths[i]);
    result.files[5] = readBytes(root.filePath(quadOutputFiles[5]), cancelled);
    result.image = opaqueDiagnostic(decodeImage(result.files[5], size, quadOutputFiles[5], cancelled), cancelled);
    result.report["experiment_key"] = contextKey.toStdString();
    result.reportText = QString::fromStdString(result.report.dump(2));
    checkCancellation(cancelled);
    return result;
}
ThumbnailOutput readThumbnailOutput(const QString &directory, const CancelCheck &cancelled) {
    const QDir root(directory);
    const auto report = nativeObject(readBytes(root.filePath("report.json"), cancelled), cancelled);
    if (!report.at("completed").is_boolean() || !report.at("completed").get<bool>())
        throw std::runtime_error("Thumbnail worker did not complete");
    if (!report.at("bindings").is_array()) throw std::runtime_error("Thumbnail bindings are not an array");
    ThumbnailOutput result;
    result.event = unsignedValue(report.at("event"), "Thumbnail event");
    std::map<QString, QImage> images;
    for (const auto &binding : report.at("bindings")) {
        checkCancellation(cancelled);
        const auto key = binding.at("key").get<std::string>();
        if (key.empty() || result.rows.contains(key)) throw std::runtime_error("Invalid or duplicate thumbnail binding key");
        ThumbnailRow row;
        row.identity = nlohmann::json::object();
        for (const auto field : {"slot", "event", "resource", "view", "format", "mip", "layer", "slice",
                                 "width", "height", "samples", "mip_end", "layer_end", "slice_end"})
            row.identity[field] = unsignedValue(binding.at(field), field);
        for (const auto field : {"key", "role", "kind", "stage", "boundary", "error"})
            row.identity[field] = binding.at(field).get<std::string>();
        row.identity["texture"] = binding.at("texture").get<bool>();
        row.identity["sample"] = binding.at("sample").is_null() ? nlohmann::json(nullptr) :
                                  nlohmann::json(unsignedValue(binding.at("sample"), "sample"));
        row.resource = unsignedValue(binding.at("resource"), "Thumbnail resource");
        row.view = unsignedValue(binding.at("view"), "Thumbnail view");
        row.event = unsignedValue(binding.at("event"), "Thumbnail binding event");
        if (row.event != result.event) throw std::runtime_error("Thumbnail binding event differs from report");
        if (binding.contains("preview") && binding.contains("preview_error"))
            throw std::runtime_error("Thumbnail has both a preview and an error");
        if (binding.contains("preview")) {
            const auto name = QString::fromStdString(binding.at("preview").get<std::string>());
            if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.contains(':') ||
                QFileInfo(name).fileName() != name || !name.endsWith(".png"))
                throw std::runtime_error("Invalid thumbnail path");
            if (const auto found = images.find(name); found != images.end()) row.display = found->second;
            else {
                const auto bytes = readBytes(root.filePath(name), cancelled);
                QBuffer buffer; buffer.setData(bytes);
                if (!buffer.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read thumbnail bytes");
                QImageReader reader(&buffer, "png");
                const auto size = reader.size();
                if (size.width() <= 0 || size.height() <= 0 || size.width() > 96 || size.height() > 96)
                    throw std::runtime_error(("Invalid thumbnail dimensions: " + name).toStdString());
                auto decoded = decodeImage(bytes, size, name.toUtf8().constData(), cancelled);
                row.display = prepareImageForDisplay(std::move(decoded), cancelled).display;
                images.emplace(name, row.display);
            }
        } else if (binding.contains("preview_error"))
            row.error = QString::fromStdString(binding.at("preview_error").get<std::string>());
        result.rows.emplace(key, std::move(row));
    }
    checkCancellation(cancelled);
    return result;
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
WorkerReport readWorkerOutput(const QString &directory, const QString &kind, const CancelCheck &cancelled,
                              const QString &diagnosticKey) {
    if (kind == "draw-resources") {
        // The native report is the complete envelope and contains precise IDs.
        // Do not parse it again through QJson or overwrite the main replay report.
        WorkerReport result;
        try { result.thumbnails = readThumbnailOutput(directory, cancelled); }
        catch (const OperationCancelled &) { throw; }
        catch (const std::exception &error) {
            throw std::runtime_error(std::string("Worker thumbnail report.json: ") + error.what());
        }
        return result;
    }
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
            if (kind == "coverage") result.coverage = readCoverageOutput(directory, cancelled);
            else if (kind == "quad") result.quad = readQuadOutput(directory, diagnosticKey, cancelled);
            else {
                const auto bytes = readBytes(QDir(directory).filePath(filename), cancelled);
                if (kind == "geometry" || kind == "post-geometry") result.geometry = qtObject(bytes, cancelled);
                else result.payload = nativeObject(bytes, cancelled);
            }
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
