#include "ImagePresentation.h"
#include <stdexcept>

namespace flora {
PreparedImage prepareImageForDisplay(QImage image, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    PreparedImage result{std::move(image), {}};
    if (!result.original.isNull()) {
        result.display = result.original.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        checkCancellation(cancelled);
        if (result.display.isNull())
            throw std::runtime_error("Cannot allocate image display conversion");
    }
    checkCancellation(cancelled);
    return result;
}
} // namespace flora
