#include "NativeArtifacts.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <set>

namespace flora {
namespace {
void validateName(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains('\\') ||
        name.contains(':') || name.contains(QChar(0)) || name.endsWith('.') || name.endsWith(' ') ||
        name == "result.json" || name == "job.json" || name == "worker.log")
        throw std::runtime_error("Invalid native artifact name");
}
void validateFile(const QFileInfo &file) {
    if (!file.isFile() || file.isSymLink() || file.isJunction())
        throw std::runtime_error("Native artifact must be a regular file");
}
} // namespace
nlohmann::json nativeArtifactManifest(const QString &directory, const CancelCheck &cancelled) {
    nlohmann::json manifest = nlohmann::json::object();
    for (const auto &file : QDir(directory).entryInfoList(
             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) {
        checkCancellation(cancelled);
        validateName(file.fileName());
        validateFile(file);
        manifest[file.fileName().toStdString()] = {
            {"bytes", file.size()}, {"sha256", analysisFileDigest(file.filePath(), cancelled)}};
    }
    return manifest;
}
static NativeArtifactBytes readArtifacts(const QString &directory, const nlohmann::json &manifest,
                                         uint64_t maximumBytes, bool retain, const CancelCheck &cancelled) {
    if (!manifest.is_object())
        throw std::runtime_error("Invalid native artifact manifest");
    uint64_t total = 0;
    std::set<QString> names;
    // Validate the entire allocation budget and all names before reading any payload.
    for (const auto &[name, metadata] : manifest.items()) {
        const auto filename = QString::fromStdString(name);
        validateName(filename);
        if (!names.insert(filename.toCaseFolded()).second)
            throw std::runtime_error("Duplicate native artifact name");
        const auto &size = metadata.at("bytes");
        if (!size.is_number_integer() || (!size.is_number_unsigned() && size.get<int64_t>() < 0))
            throw std::runtime_error("Invalid native artifact size");
        const auto bytes = size.get<uint64_t>();
        if (bytes > maximumBytes - total || bytes > uint64_t(QByteArray::maxSize()))
            throw std::runtime_error("Native artifact memory budget exceeded");
        total += bytes;
        const auto hash = metadata.at("sha256").get<std::string>();
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid native artifact digest");
    }
    NativeArtifactBytes result;
    for (const auto &[name, metadata] : manifest.items()) {
        checkCancellation(cancelled);
        const auto filename = QString::fromStdString(name), path = QDir(directory).filePath(filename);
        validateFile(QFileInfo(path));
        QFile file(path);
        const auto expected = metadata.at("bytes").get<qint64>();
        if (!file.open(QIODevice::ReadOnly) || file.size() != expected)
            throw std::runtime_error("Native artifact is missing or has changed size");
        QByteArray bytes;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 consumed = 0;
        while (consumed < expected) {
            checkCancellation(cancelled);
            const auto chunk = file.read((std::min)(qint64(4 * 1024 * 1024), expected - consumed));
            if (chunk.isEmpty() || file.error() != QFileDevice::NoError)
                throw std::runtime_error("Cannot read complete native artifact");
            hash.addData(chunk);
            consumed += chunk.size();
            if (retain)
                bytes.append(chunk);
        }
        if (!file.atEnd() || file.size() != expected ||
            hash.result().toHex().toStdString() != metadata.at("sha256").get<std::string>())
            throw std::runtime_error("Native artifact digest mismatch");
        if (retain)
            result.insert(filename, std::move(bytes));
    }
    checkCancellation(cancelled);
    return result;
}
NativeArtifactBytes readNativeArtifacts(const QString &directory, const nlohmann::json &manifest,
                                        uint64_t maximumBytes, const CancelCheck &cancelled) {
    return readArtifacts(directory, manifest, maximumBytes, true, cancelled);
}
void verifyNativeArtifacts(const QString &directory, const nlohmann::json &manifest,
                           const CancelCheck &cancelled) {
    readArtifacts(directory, manifest, UINT64_MAX, false, cancelled);
}
} // namespace flora
