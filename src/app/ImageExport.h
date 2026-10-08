#pragma once
#include "core/Frame.h"
#include <QImage>
#include <QString>
namespace flora {
// Callers retain the requested image/cache independently of UI selection.
void exportImageFile(const QString &path, const QImage &image, const CancelCheck &cancelled = {});
void copyExportFile(const QString &source, const QString &path, const CancelCheck &cancelled = {});
}
