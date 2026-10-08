#include "GeometryExport.h"
#include "ImageExport.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <Windows.h>

namespace flora {
std::vector<GeometryExportAsset> geometryExportAssets(const QString &source) {
    QDir directory(source);
    if (!directory.exists()) throw std::runtime_error("Geometry export cache is unavailable");
    std::vector<GeometryExportAsset> assets;
    const auto files = directory.entryInfoList(
        {"*.csv", "geometry.json", "geometry.obj", "vertices.bin", "vertices.validity.bin",
         "unique_vertices.bin", "patch_constants.bin", "patch_constants.validity.bin"}, QDir::Files, QDir::Name);
    bool report = false;
    for (const auto &file : files) {
        if (file.isSymLink() || !file.isFile() || file.size() < 0)
            throw std::runtime_error("Geometry export asset is not a regular file");
        report |= file.fileName() == "geometry.json";
        assets.push_back({file.fileName(), file.size()});
    }
    if (!report) throw std::runtime_error("Geometry export report is unavailable");
    return assets;
}
QString exportGeometryDirectory(const QString &source, const std::vector<GeometryExportAsset> &assets,
                                quint64 event, const QString &root, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (!event || !QFileInfo(root).isDir()) throw std::runtime_error("Invalid geometry export destination");
    QSet<QString> names;
    for (const auto &asset : assets) {
        if (asset.name.isEmpty() || asset.name == "." || asset.name == ".." ||
            asset.name.contains('/') || asset.name.contains('\\') || asset.name.contains(':') ||
            asset.size < 0 || names.contains(asset.name.toCaseFolded()))
            throw std::runtime_error("Invalid geometry export inventory");
        names.insert(asset.name.toCaseFolded());
    }
    if (!names.contains("geometry.json")) throw std::runtime_error("Geometry export report is unavailable");
    const QDir destination(QFileInfo(root).absoluteFilePath());
    QTemporaryDir staging(destination.filePath(".FloraGPA-Geometry-XXXXXX"));
    if (!staging.isValid()) throw std::runtime_error("Cannot stage geometry export");
    for (const auto &asset : assets) {
        checkCancellation(cancelled);
        const auto input = QDir(source).filePath(asset.name);
        const QFileInfo info(input);
        if (!info.isFile() || info.isSymLink() || info.size() != asset.size)
            throw std::runtime_error("Geometry export asset is missing or changed");
        const auto output = staging.filePath(asset.name);
        copyExportFile(input, output, cancelled);
        if (QFileInfo(output).size() != asset.size)
            throw std::runtime_error("Geometry export asset length changed");
    }
    // Same-parent directory rename publishes the complete set without replacing
    // any existing directory. Do not fall back to per-file publication/copying.
    const auto base = QString("FloraGPA-Geometry-%1").arg(event);
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        checkCancellation(cancelled);
        const auto target = destination.filePath(base + (suffix ? '-' + QString::number(suffix) : QString{}));
        if (MoveFileExW(reinterpret_cast<const wchar_t *>(staging.path().utf16()),
                        reinterpret_cast<const wchar_t *>(target.utf16()), 0)) {
            staging.setAutoRemove(false);
            return target;
        }
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            throw std::runtime_error("Cannot publish geometry export directory");
    }
    throw std::runtime_error("Geometry export directory names are exhausted");
}
}
