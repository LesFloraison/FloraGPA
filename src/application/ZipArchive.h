#pragma once
#include <QByteArray>
#include <QString>
#include <vector>
namespace flora {
struct ZipEntry {
    QString name;
    QString source;
    QByteArray bytes;
};
// Store entries without recompressing already generated capture artifacts.
// Streams source files and writes ZIP64 metadata when sizes/offsets require it.
void writeZipArchive(const QString &destination, const std::vector<ZipEntry> &entries);
} // namespace flora
