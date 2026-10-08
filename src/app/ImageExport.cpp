#include "ImageExport.h"
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>
#include <algorithm>
#include <exception>

namespace flora {
namespace {
// Never throw cancellation through an image codec's C callbacks. Report a
// failed device write, then raise cancellation after the codec returns.
class ImageSink final : public QIODevice {
  public:
    ImageSink(QSaveFile &file, const CancelCheck &cancelled) : file_(file), cancelled_(cancelled) {
        open(QIODevice::WriteOnly | QIODevice::Unbuffered);
    }
    bool cancelled = false, failed = false;
    std::exception_ptr callbackError;
    bool isSequential() const override { return true; }
  protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 length) override {
        if (cancelled || failed) return -1;
        qint64 written = 0;
        while (written < length) {
            try {
                if (cancelled_ && cancelled_()) { cancelled = true; return -1; }
            } catch (...) {
                callbackError = std::current_exception(); failed = true; return -1;
            }
            const auto size = std::min<qint64>(1024 * 1024, length - written);
            if (file_.write(data + written, size) != size) { failed = true; return -1; }
            written += size;
        }
        return written;
    }
  private:
    QSaveFile &file_;
    const CancelCheck &cancelled_;
};
}
void exportImageFile(const QString &path, const QImage &image, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    if (image.isNull()) throw std::runtime_error("No image to export");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot stage image export");
    ImageSink sink(file, cancelled);
    // Match QImage::save(path)'s suffix-based format selection.
    QImageWriter writer(&sink, QFileInfo(path).suffix().toLatin1().toLower());
    const bool saved = writer.write(image);
    if (sink.callbackError) std::rethrow_exception(sink.callbackError);
    if (sink.cancelled) throw OperationCancelled();
    checkCancellation(cancelled);
    if (!saved || sink.failed) throw std::runtime_error("Cannot encode complete image export");
    if (!file.commit()) throw std::runtime_error("Cannot publish image export");
}
void copyExportFile(const QString &source, const QString &path, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Export asset is unavailable");
    const auto size = input.size();
    if (size < 0) throw std::runtime_error("Export asset length is invalid");
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot stage asset export");
    QByteArray chunk(1024 * 1024, Qt::Uninitialized);
    qint64 copied = 0;
    while (copied < size) {
        checkCancellation(cancelled);
        const auto read = input.read(chunk.data(), std::min<qint64>(chunk.size(), size - copied));
        if (read <= 0 || input.error() != QFileDevice::NoError)
            throw std::runtime_error("Cannot read complete export asset");
        if (output.write(chunk.constData(), read) != read)
            throw std::runtime_error("Cannot stage complete export asset");
        copied += read;
    }
    if (input.error() != QFileDevice::NoError || input.pos() != size || input.size() != size)
        throw std::runtime_error("Export asset length changed");
    input.close();
    checkCancellation(cancelled);
    if (!output.commit()) throw std::runtime_error("Cannot publish export asset");
}
}
