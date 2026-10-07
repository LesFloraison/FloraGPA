#pragma once
#include "core/Cancellation.h"
#include <QImage>

namespace flora {
struct PreparedImage {
    QImage original;
    QImage display;
};
// QImage-only work: safe in the image-validation task, never creates a pixmap.
// Preserve straight RGBA data separately from Qt's premultiplied paint format.
PreparedImage prepareImageForDisplay(QImage image, const CancelCheck &cancelled = {});
} // namespace flora
