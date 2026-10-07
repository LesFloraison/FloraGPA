#include "WorkerBuffer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace flora {
namespace {
uint64_t byteCount(const QJsonValue &value) {
    const auto n = value.toDouble(-1);
    // A DX11 buffer has a UINT ByteWidth. Keeping this bound also prevents
    // floating-point rounding and narrowing into model row counts.
    if (!value.isDouble() || !std::isfinite(n) || n < 0 || std::floor(n) != n ||
        n > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Worker buffer range is invalid");
    return uint64_t(n);
}
} // namespace
QByteArray readWorkerBuffer(const QString &directory, const QJsonObject &report,
                          const BufferRequest &request, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const auto max = std::numeric_limits<uint32_t>::max();
    if (!request.resource || request.offset > max || request.length > max - request.offset)
        throw std::runtime_error("Worker buffer request range is invalid");
    const auto when = request.event ? (request.before ? "before_event" : "after_event") : "capture_initial";
    if (!report["completed"].isBool() || !report["completed"].toBool() ||
        report["resource"] != QJsonValue(QString::number(request.resource)) ||
        byteCount(report["offset"]) != request.offset || byteCount(report["length"]) != request.length ||
        report["value_time"] != QJsonValue(when) ||
        (request.event ? report["event"] != QJsonValue(QString::number(request.event))
                       : !report["event"].isNull()))
        throw std::runtime_error("Worker buffer report does not match the request");
    const auto hash = report["sha256"].toString();
    static const QRegularExpression sha256("^[0-9a-f]{64}$");
    if (!sha256.match(hash).hasMatch())
        throw std::runtime_error("Worker buffer hash is missing or invalid");
    QFile file(QDir(directory).filePath("buffer.bin"));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Worker buffer output is missing or unreadable");
    const auto length = qint64(request.length);
    if (file.size() != length)
        throw std::runtime_error("Worker buffer length does not match the report");
    // Append bounded chunks instead of allocating an untrusted file size in
    // one call. The returned bytes are exactly the bytes whose hash is checked.
    QByteArray bytes;
    QCryptographicHash actual(QCryptographicHash::Sha256);
    while (bytes.size() < length) {
        checkCancellation(cancelled);
        const auto chunk = file.read(std::min<qint64>(1024 * 1024, length - bytes.size()));
        if (chunk.isEmpty() || file.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot read complete worker buffer output");
        actual.addData(chunk);
        bytes.append(chunk);
    }
    checkCancellation(cancelled);
    if (file.pos() != length || file.size() != length || file.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read complete worker buffer output");
    if (actual.result().toHex() != hash.toLatin1())
        throw std::runtime_error("Worker buffer hash does not match the report");
    return bytes;
}
} // namespace flora
