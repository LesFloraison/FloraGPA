#include "WorkerImage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace flora {
namespace {
int dimension(const QJsonValue &value) {
    const auto n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || n < 1 || std::floor(n) != n ||
        n > std::numeric_limits<int>::max() / 4)
        throw std::runtime_error("Worker image dimensions are invalid");
    return int(n);
}
} // namespace
QImage readWorkerImage(const QString &directory, const QJsonObject &report, bool replay,
                       const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    const QDir root(directory);
    const auto png = root.filePath("frame.png"), rawPath = root.filePath("frame.rgba");
    if (replay && !report["image_available"].isBool())
        throw std::runtime_error("Worker image availability is invalid");
    if (replay && !report["image_available"].toBool()) {
        for (const auto key : {"width", "height", "resource", "rgba_sha256"})
            if (!report[key].isNull())
                throw std::runtime_error("Worker no-output report contains image metadata");
        if (QFileInfo::exists(png) || QFileInfo::exists(rawPath))
            throw std::runtime_error("Worker no-output report contains image files");
        return {};
    }
    const QSize size(dimension(report["width"]), dimension(report["height"]));
    const auto expectedHash = report["rgba_sha256"].toString().toLatin1();
    static const QRegularExpression sha256("^[0-9a-f]{64}$");
    if (!sha256.match(QString::fromLatin1(expectedHash)).hasMatch())
        throw std::runtime_error("Worker image hash is missing or invalid");
    QImageReader reader(png, "png");
    if (reader.size() != size)
        throw std::runtime_error("Worker PNG dimensions do not match the report");
    checkCancellation(cancelled);
    // Hash the raw file without allocating a second full-sized image buffer.
    const auto length = qint64(size.width()) * size.height() * 4;
    QFile raw(rawPath);
    if (!raw.open(QIODevice::ReadOnly))
        throw std::runtime_error("Worker RGBA output is missing or unreadable");
    if (raw.size() != length)
        throw std::runtime_error("Worker RGBA length does not match the report");
    QCryptographicHash rawHash(QCryptographicHash::Sha256);
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    while (raw.pos() < length) {
        checkCancellation(cancelled);
        const auto count = raw.read(chunk.data(), std::min<qint64>(chunk.size(), length - raw.pos()));
        if (count <= 0)
            throw std::runtime_error("Cannot read complete worker RGBA output");
        rawHash.addData(QByteArrayView(chunk.constData(), count));
    }
    checkCancellation(cancelled);
    if (raw.pos() != length || raw.size() != length || raw.error() != QFileDevice::NoError)
        throw std::runtime_error("Cannot read complete worker RGBA output");
    if (rawHash.result().toHex() != expectedHash)
        throw std::runtime_error("Worker RGBA hash does not match the report");
    auto image = reader.read();
    checkCancellation(cancelled);
    if (image.isNull() || image.size() != size)
        throw std::runtime_error("Worker PNG output is incomplete or unreadable");
    image = image.convertToFormat(QImage::Format_RGBA8888);
    checkCancellation(cancelled);
    if (image.isNull())
        throw std::runtime_error("Cannot allocate worker image conversion");
    QCryptographicHash imageHash(QCryptographicHash::Sha256);
    for (int y = 0; y < image.height(); ++y) {
        checkCancellation(cancelled);
        imageHash.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(y)),
                                         qsizetype(image.width()) * 4));
    }
    checkCancellation(cancelled);
    if (imageHash.result().toHex() != expectedHash)
        throw std::runtime_error("Worker PNG pixels do not match the report");
    return image;
}
} // namespace flora
