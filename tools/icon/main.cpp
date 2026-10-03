// Development-only SVG to multi-resolution Windows ICO conversion.
#include <QBuffer>
#include <QDataStream>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSaveFile>
#include <QSvgRenderer>
#include <array>
#include <cstdio>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (app.arguments().size() != 3) {
        std::fprintf(stderr, "Usage: FloraIconGenerator <source.svg> <output.ico>\n");
        return 2;
    }
    QSvgRenderer svg(app.arguments()[1]);
    if (!svg.isValid())
        return 1;
    constexpr std::array sizes{16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::array<QByteArray, sizes.size()> images;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const auto size = sizes[i];
        QImage image(size * 4, size * 4, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            svg.render(&painter);
        }
        image = image.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QBuffer buffer(&images[i]);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return 1;
    }
    QSaveFile file(app.arguments()[2]);
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * sizes.size();
    for (size_t i = 0; i < sizes.size(); ++i) {
        const auto dimension = quint8(sizes[i] == 256 ? 0 : sizes[i]);
        stream << dimension << dimension << quint8(0) << quint8(0) << quint16(1) << quint16(32)
               << quint32(images[i].size()) << offset;
        offset += quint32(images[i].size());
    }
    for (const auto &image : images)
        if (stream.writeRawData(image.data(), image.size()) != image.size())
            return 1;
    return stream.status() == QDataStream::Ok && file.commit() ? 0 : 1;
}
