#include "ZipArchive.h"
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace flora {
namespace {
void append(QByteArray &out, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        out.append(char(value >> (i * 8)));
}
void write(QIODevice &out, const QByteArray &bytes) {
    if (out.write(bytes) != bytes.size())
        throw std::runtime_error("Cannot write checkpoint archive");
}
uint32_t crc(uint32_t value, const QByteArray &bytes) {
    static const auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            auto v = i;
            for (unsigned j = 0; j < 8; ++j)
                v = (v >> 1) ^ (v & 1 ? 0xedb88320u : 0u);
            result[i] = v;
        }
        return result;
    }();
    for (const auto byte : bytes)
        value = table[(value ^ uint8_t(byte)) & 255] ^ (value >> 8);
    return value;
}
} // namespace
void writeZipArchive(const QString &destination, const std::vector<ZipEntry> &entries) {
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create checkpoint archive");
    QByteArray central;
    std::set<QString> names;
    for (const auto &entry : entries) {
        const auto name = entry.name.toUtf8();
        if (name.isEmpty() || name.size() > 65535 || entry.name.contains('/') || entry.name.contains('\\') ||
            entry.name == "." || entry.name == ".." || name.contains('\0') ||
            !names.insert(entry.name).second)
            throw std::runtime_error("Invalid checkpoint archive member name");
        QFile input(entry.source);
        if (!entry.source.isEmpty() && !input.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read checkpoint archive member");
        const auto length = input.isOpen() ? input.size() : entry.bytes.size();
        if (length < 0)
            throw std::runtime_error("Cannot read checkpoint archive member size");
        const auto size = uint64_t(length), offset = uint64_t(output.pos());
        const bool wide = size >= UINT32_MAX, wideOffset = offset >= UINT32_MAX;
        QByteArray extra;
        if (wide) {
            append(extra, 1, 2);
            append(extra, 16, 2);
            append(extra, size, 8);
            append(extra, size, 8);
        }
        QByteArray header;
        append(header, 0x04034b50, 4);
        append(header, wide ? 45 : 20, 2);
        append(header, 0x808, 2);
        append(header, 0, 2); // UTF-8 + descriptor; STORE
        append(header, 0, 2);
        append(header, 33, 2); // 1980-01-01
        append(header, 0, 4);
        append(header, wide ? UINT32_MAX : 0, 4);
        append(header, wide ? UINT32_MAX : 0, 4);
        append(header, uint64_t(name.size()), 2);
        append(header, uint64_t(extra.size()), 2);
        write(output, header);
        write(output, name);
        write(output, extra);
        uint32_t checksum = UINT32_MAX;
        uint64_t copied = 0;
        if (input.isOpen()) {
            while (copied < size) {
                const auto bytes = input.read(qint64(std::min(uint64_t(1024 * 1024), size - copied)));
                if (bytes.isEmpty() || input.error() != QFileDevice::NoError)
                    throw std::runtime_error("Truncated checkpoint archive member");
                checksum = crc(checksum, bytes);
                copied += bytes.size();
                write(output, bytes);
            }
            if (!input.atEnd())
                throw std::runtime_error("Checkpoint archive member changed while exporting");
        } else {
            checksum = crc(checksum, entry.bytes);
            write(output, entry.bytes);
        }
        checksum ^= UINT32_MAX;
        QByteArray descriptor;
        append(descriptor, 0x08074b50, 4);
        append(descriptor, checksum, 4);
        append(descriptor, size, wide ? 8 : 4);
        append(descriptor, size, wide ? 8 : 4);
        write(output, descriptor);
        extra.clear();
        if (wide || wideOffset) {
            append(extra, 1, 2);
            append(extra, (wide ? 16 : 0) + (wideOffset ? 8 : 0), 2);
            if (wide) {
                append(extra, size, 8);
                append(extra, size, 8);
            }
            if (wideOffset)
                append(extra, offset, 8);
        }
        append(central, 0x02014b50, 4);
        append(central, 45, 2);
        append(central, wide || wideOffset ? 45 : 20, 2);
        append(central, 0x808, 2);
        append(central, 0, 2);
        append(central, 0, 2);
        append(central, 33, 2);
        append(central, checksum, 4);
        append(central, wide ? UINT32_MAX : size, 4);
        append(central, wide ? UINT32_MAX : size, 4);
        append(central, uint64_t(name.size()), 2);
        append(central, uint64_t(extra.size()), 2);
        append(central, 0, 2);
        append(central, 0, 2);
        append(central, 0, 2);
        append(central, 0, 4);
        append(central, wideOffset ? UINT32_MAX : offset, 4);
        central += name;
        central += extra;
    }
    const auto offset = uint64_t(output.pos()), length = uint64_t(central.size());
    write(output, central);
    if (offset >= UINT32_MAX || length >= UINT32_MAX || entries.size() >= UINT16_MAX) {
        const auto end = uint64_t(output.pos());
        QByteArray record;
        append(record, 0x06064b50, 4);
        append(record, 44, 8);
        append(record, 45, 2);
        append(record, 45, 2);
        append(record, 0, 4);
        append(record, 0, 4);
        append(record, entries.size(), 8);
        append(record, entries.size(), 8);
        append(record, length, 8);
        append(record, offset, 8);
        append(record, 0x07064b50, 4);
        append(record, 0, 4);
        append(record, end, 8);
        append(record, 1, 4);
        write(output, record);
    }
    QByteArray end;
    append(end, 0x06054b50, 4);
    append(end, 0, 2);
    append(end, 0, 2);
    append(end, std::min(uint64_t(entries.size()), uint64_t(UINT16_MAX)), 2);
    append(end, std::min(uint64_t(entries.size()), uint64_t(UINT16_MAX)), 2);
    append(end, std::min(length, uint64_t(UINT32_MAX)), 4);
    append(end, std::min(offset, uint64_t(UINT32_MAX)), 4);
    append(end, 0, 2);
    write(output, end);
    if (!output.commit())
        throw std::runtime_error("Cannot commit checkpoint archive");
}
} // namespace flora
