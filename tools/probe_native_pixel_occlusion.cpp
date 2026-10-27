#include "application/NativePixelOcclusion.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <iostream>
using namespace flora;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        const auto a = app.arguments();
        if (a.size() != 9)
            throw std::runtime_error(
                "Usage: FloraNativePixelOcclusionProbe FRAME EVENT SLOT X Y SAMPLE VENDOR OUTPUT");
        auto number = [&](int i) {
            bool ok = false;
            const auto value = a[i].toUInt(&ok, 0);
            if (!ok)
                throw std::runtime_error("Invalid occlusion probe number");
            return value;
        };
        Frame frame(a[1].toStdWString());
        ReplayOptions options;
        options.debug = true;
        const auto vendor = number(7);
        options.warp = !vendor;
        if (vendor)
            options.vendor = vendor;
        bool eventOk = false;
        const auto event = a[2].toULongLong(&eventOk, 0);
        if (!eventOk)
            throw std::runtime_error("Invalid occlusion probe event identity");
        NativePixelOcclusionSelection s{event, number(3), number(4), number(5), number(6)};
        QElapsedTimer timer;
        timer.start();
        auto result = measureNativePixelOcclusion(frame, options, s);
        result["elapsed_ms"] = timer.elapsed();
        QFile out(a[8]);
        const auto text = result.dump(2);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            out.write(text.data(), qint64(text.size())) != qint64(text.size()))
            throw std::runtime_error("Cannot write occlusion probe output");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
