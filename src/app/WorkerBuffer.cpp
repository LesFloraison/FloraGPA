#include "WorkerBuffer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <charconv>

namespace flora {
namespace {
uint64_t byteCount(const QJsonValue &value, bool dx12) {
    if (dx12) {
        if (!value.isString()) throw std::runtime_error("DX12 buffer range must be a decimal string");
        const auto text = value.toString().toStdString(); uint64_t result{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || text != std::to_string(result))
            throw std::runtime_error("DX12 buffer range is invalid");
        return result;
    }
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
    const auto max = request.dx12 ? UINT64_MAX : uint64_t(std::numeric_limits<uint32_t>::max());
    if (!request.resource || request.offset > max || request.length > max - request.offset)
        throw std::runtime_error("Worker buffer request range is invalid");
    if (request.dx12 && (!request.event || !request.length || request.length > 256ULL * 1024 * 1024 || report["backend"] != "dx12"))
        throw std::runtime_error("Invalid DX12 buffer request/backend");
    if (!request.dx12 && report["backend"] == "dx12") throw std::runtime_error("Unexpected DX12 buffer result");
    if (request.dx12 && report["execution"] != QJsonValue(QString::number(request.event)))
        throw std::runtime_error("DX12 buffer execution does not match request");
    const auto when = request.event ? (request.before ? "before_event" : "after_event") : "capture_initial";
    if (!report["completed"].isBool() || !report["completed"].toBool() ||
        report["resource"] != QJsonValue(QString::number(request.resource)) ||
        byteCount(report["offset"], request.dx12) != request.offset || byteCount(report["length"], request.dx12) != request.length ||
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
