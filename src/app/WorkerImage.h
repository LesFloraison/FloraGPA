#pragma once
#include "core/Cancellation.h"
#include <QImage>
#include <QJsonObject>
#include <QString>

namespace flora {
// Validate the completed worker's display artifacts before replacing any UI
// output. A null image is allowed only for an explicit replay no-output result.
QImage readWorkerImage(const QString &directory, const QJsonObject &report, bool replay,
                       const CancelCheck &cancelled = {});
} // namespace flora
