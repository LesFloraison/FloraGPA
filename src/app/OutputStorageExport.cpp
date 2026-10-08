#include "OutputStorageExport.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <limits>

namespace flora {
namespace {
QString identity(const QString &path) {
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical).toCaseFolded();
}
}
void exportOutputStorageFiles(const QString &directory, const QString &destination,
                              const nlohmann::json &display, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (!display.is_object() || !display.contains("storage_bytes") ||
        !display["storage_bytes"].is_number_integer() ||
        (!display["storage_bytes"].is_number_unsigned() && display["storage_bytes"].get<int64_t>() < 0))
        throw std::runtime_error("Output storage byte count is invalid");
    const auto size = display["storage_bytes"].get<uint64_t>();
    if (size > uint64_t(std::numeric_limits<qint64>::max()))
        throw std::runtime_error("Output storage byte count exceeds file limits");
    if (!display.contains("storage_sha256") || !display["storage_sha256"].is_string())
        throw std::runtime_error("Output storage hash is missing");
    const auto expectedHash = QByteArray::fromStdString(display["storage_sha256"].get<std::string>());
    static const QRegularExpression hashPattern("^[0-9a-f]{64}$");
    if (!hashPattern.match(QString::fromLatin1(expectedHash)).hasMatch())
        throw std::runtime_error("Output storage hash is invalid");
    const auto rawPath = QDir(directory).filePath("output_storage.bin");
    const auto metadataPath = QDir(directory).filePath("output_storage.json");
    for (const auto &source : {rawPath, metadataPath})
        for (const auto &target : {destination, destination + ".json"})
            if (identity(source) == identity(target))
                throw std::runtime_error("Export destination overlaps the Worker storage");
    // The Worker emits this exact serialization from the same native object
    // saved in report.json. Check before allocating or trusting a sidecar body.
    const auto expectedMetadata = QByteArray::fromStdString(display.dump(2) + "\n");
    QFile metadata(metadataPath);
    if (!metadata.open(QIODevice::ReadOnly) || metadata.size() != expectedMetadata.size())
        throw std::runtime_error("Output storage metadata is missing or has an unexpected length");
    const auto bytes = metadata.read(expectedMetadata.size());
    if (metadata.error() != QFileDevice::NoError || metadata.size() != expectedMetadata.size() ||
        metadata.pos() != expectedMetadata.size() || bytes != expectedMetadata)
        throw std::runtime_error("Output storage metadata does not match the accepted report");
    metadata.close();
    checkCancellation(cancelled);
    QFile raw(rawPath);
    if (!raw.open(QIODevice::ReadOnly) || raw.size() != qint64(size))
        throw std::runtime_error("Output storage length does not match the accepted report");
    QSaveFile binary(destination), sidecar(destination + ".json");
    // Never enable direct-write fallback: validation/cancellation must not
    // truncate an existing destination.
    if (!binary.open(QIODevice::WriteOnly) || !sidecar.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot stage output storage export");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    qint64 copied = 0;
    while (copied < qint64(size)) {
        checkCancellation(cancelled);
        const auto count = raw.read(chunk.data(), std::min<qint64>(chunk.size(), qint64(size) - copied));
        if (count <= 0 || raw.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot read complete output storage");
        if (binary.write(chunk.constData(), count) != count)
            throw std::runtime_error("Cannot stage complete output storage");
        hash.addData(QByteArrayView(chunk.constData(), count));
        copied += count;
    }
    if (raw.error() != QFileDevice::NoError || raw.pos() != qint64(size) || raw.size() != qint64(size) ||
        hash.result().toHex() != expectedHash)
        throw std::runtime_error("Output storage bytes do not match the accepted report");
    raw.close();
    if (sidecar.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot stage output storage metadata");
    // Cancellation is honored until publication starts. Do not abandon the
    // second rename after successfully publishing the binary.
    checkCancellation(cancelled);
    if (!binary.commit()) throw std::runtime_error("Cannot publish output storage");
    if (!sidecar.commit())
        throw std::runtime_error("Output storage was saved, but metadata publication failed");
}
}
