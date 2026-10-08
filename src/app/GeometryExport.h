#pragma once
#include "core/Frame.h"
#include <QString>
#include <vector>
namespace flora {
struct GeometryExportAsset {
    QString name;
    qint64 size;
};
// Snapshot names and lengths before opening a nested file chooser. The caller
// retains the accepted cache directory until the background operation ends.
std::vector<GeometryExportAsset> geometryExportAssets(const QString &source);
QString exportGeometryDirectory(const QString &source, const std::vector<GeometryExportAsset> &assets,
                                quint64 event, const QString &root, const CancelCheck &cancelled = {});
}
