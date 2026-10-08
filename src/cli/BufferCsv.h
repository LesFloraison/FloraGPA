#pragma once
#include <QByteArrayView>
#include <QString>
class QIODevice;
namespace flora {
// The input remains owned by the caller. Only a bounded CSV chunk is retained.
void writeBufferCsv(QIODevice &output, QByteArrayView bytes, quint64 offset);
void saveBufferCsv(const QString &path, QByteArrayView bytes, quint64 offset);
}
