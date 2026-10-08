#pragma once
#include "core/Frame.h"
#include <QString>

namespace flora {
// The caller retains the immutable storage for the duration of this call.
// Replacement starts only after all bytes are staged and cancellation checked.
void exportBytesFile(const QString &destination, Bytes bytes, const CancelCheck &cancelled = {});
}
