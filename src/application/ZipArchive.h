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
// Member names may use relative forward-slash paths; empty, dot, parent,
// absolute, drive-qualified and duplicate paths are rejected.
// Streams source files and writes ZIP64 metadata when sizes/offsets require it.
void writeZipArchive(const QString &destination, const std::vector<ZipEntry> &entries);
} // namespace flora
