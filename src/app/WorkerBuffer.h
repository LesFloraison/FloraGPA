#pragma once
#include "core/Frame.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace flora {
struct BufferRequest {
    Id resource{};
    uint64_t offset{}, length{};
    Id event{};
    bool before{};
    bool dx12{};
};
QByteArray readWorkerBuffer(const QString &directory, const QJsonObject &report,
                          const BufferRequest &request, const CancelCheck &cancelled = {});
} // namespace flora
