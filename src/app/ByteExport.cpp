#include "ByteExport.h"
#include <QSaveFile>
#include <limits>

namespace flora {
void exportBytesFile(const QString &destination, Bytes bytes, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (bytes.size() > uint64_t(std::numeric_limits<qint64>::max()))
        throw std::runtime_error("Export exceeds file limits");
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot stage byte export");
    size_t copied = 0;
    while (copied < bytes.size()) {
        checkCancellation(cancelled);
        const auto count = std::min<size_t>(1024 * 1024, bytes.size() - copied);
        if (file.write(reinterpret_cast<const char *>(bytes.data() + copied), qint64(count)) != qint64(count))
            throw std::runtime_error("Cannot stage complete byte export");
        copied += count;
    }
    checkCancellation(cancelled);
    // Keep QSaveFile's direct-write fallback disabled. A failed rename or an
    // earlier cancellation must preserve the existing destination.
    if (!file.commit()) throw std::runtime_error("Cannot publish byte export");
}
}
