#include "BufferCsv.h"
#include <QByteArray>
#include <QIODevice>
#include <QSaveFile>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace flora {
void writeBufferCsv(QIODevice &output, QByteArrayView bytes, quint64 offset) {
    if (!bytes.empty() && quint64(bytes.size() - 1) > std::numeric_limits<quint64>::max() - offset)
        throw std::invalid_argument("Buffer CSV byte range overflows");
    constexpr qsizetype chunkSize = 1024 * 1024;
    QByteArray chunk = "byte_offset,hex_bytes,uint32,int32,float32\n";
    chunk.reserve(chunkSize);
    const auto flush = [&] {
        if (output.write(chunk) != chunk.size())
            throw std::runtime_error("Cannot write complete buffer CSV");
        chunk.resize(0);
    };
    for (qsizetype pos = 0; pos < bytes.size();) {
        const auto count = std::min<qsizetype>(4, bytes.size() - pos);
        const auto word = QByteArray::fromRawData(bytes.data() + pos, count);
        auto row = QByteArray::number(offset + quint64(pos)) + ',' + word.toHex();
        if (count == 4) {
            uint32_t u;
            int32_t i;
            float f;
            std::memcpy(&u, word.constData(), 4);
            std::memcpy(&i, word.constData(), 4);
            std::memcpy(&f, word.constData(), 4);
            row += ',' + QByteArray::number(u) + ',' + QByteArray::number(i) + ',' +
                   QByteArray::number(double(f), 'g', 17);
        } else row += ",,,";
        row += '\n';
        if (chunk.size() + row.size() > chunkSize) flush();
        chunk += row;
        pos += count;
    }
    if (!chunk.isEmpty()) flush();
}
void saveBufferCsv(const QString &path, QByteArrayView bytes, quint64 offset) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot stage buffer CSV");
    writeBufferCsv(file, bytes, offset);
    // Direct-write fallback remains disabled, preserving an existing target
    // when staging or final replacement fails.
    if (!file.commit()) throw std::runtime_error("Cannot publish buffer CSV");
}
}
