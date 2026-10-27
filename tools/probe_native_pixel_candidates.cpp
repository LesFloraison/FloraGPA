#include "application/NativePixelCandidates.h"
#include "core/StreamOutput.h"
#include <QCoreApplication>
#include <QDir>
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
                "Usage: FloraNativePixelCandidatesProbe FRAME EVENT X Y SAMPLE LAYER VENDOR NEW_DIRECTORY");
        auto number = [&](int i) {
            bool ok = false;
            const auto n = a[i].toULongLong(&ok, 0);
            if (!ok)
                throw std::runtime_error("Invalid candidate probe number");
            return n;
        };
        const Id event = number(2);
        for (int i = 3; i <= 7; ++i)
            if (number(i) > UINT32_MAX)
                throw std::runtime_error("Candidate selection exceeds uint32");
        const auto vendor = uint32_t(number(7));
        QDir dir(a[8]);
        if (dir.exists() || !QDir().mkpath(a[8]))
            throw std::runtime_error("Candidate output directory must be new");
        auto save = [&](const QString &name, Bytes bytes) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                f.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                    qint64(bytes.size()))
                throw std::runtime_error("Cannot save candidate evidence");
        };
        QElapsedTimer timer;
        timer.start();
        const CancelCheck cancelled = [&] { return timer.elapsed() > 120000; };
        Frame frame(a[1].toStdWString());
        ReplayOptions options;
        options.debug = true;
        options.until = event;
        options.before = options.prepareBeforeDraw = true;
        options.warp = !vendor;
        if (vendor)
            options.vendor = vendor;
        Replay replay(frame, options);
        replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); });
        dxbc_exec::PixelRasterState raster;
        replay.inspectNativeState(
            [&](auto *context, const auto &) { raster = dxbc_exec::pixelRasterState(context); });
        const auto state = frame.state(frame.event(event).state);
        PostTransformOptions geometryOptions;
        if (state.stages[3].shader)
            if (const auto declaration = shaderStreamOutput(frame, state.stages[3].shader))
                geometryOptions.stream = readStreamOutputDeclaration(frame, declaration).rasterizedStream;
        if (geometryOptions.stream == UINT32_MAX)
            throw std::runtime_error("Draw has no rasterized stream");
        auto geometry = inspectPixelGeometry(replay, event, geometryOptions.stream, geometryOptions.maxBytes);
        NativePixelCandidateSelection selection;
        selection.x = uint32_t(number(3));
        selection.y = uint32_t(number(4));
        selection.sample = uint32_t(number(5));
        selection.layer = uint32_t(number(6));
        selection.primitiveCount = geometry.report.at("primitives").get<uint64_t>();
        auto result = captureNativePixelCandidates(replay.nativeDevice(), !vendor, raster, geometry,
                                                   selection, cancelled);
        result.report["frame_sha256"] = frame.sha256(cancelled);
        result.report["event"] = event;
        result.report["elapsed_ms"] = timer.elapsed();
        result.report["records_sha256"] = sha256(result.observations.storage);
        save("source-geometry.bin", geometry.bytes);
        save("primitive-ids.bin", Bytes(reinterpret_cast<const uint8_t *>(geometry.primitiveIds.data()),
                                        geometry.primitiveIds.size() * sizeof(uint32_t)));
        save("records.bin", result.observations.storage);
        const auto text = result.report.dump(2);
        save("report.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
