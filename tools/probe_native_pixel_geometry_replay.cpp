#include "application/NativePixelGeometryReplay.h"
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
        if (a.size() != 6)
            throw std::runtime_error(
                "Usage: FloraNativePixelGeometryReplayProbe FRAME EVENT PRIMITIVES VENDOR NEW_DIRECTORY");
        auto number = [&](int i) {
            bool ok = false;
            const auto n = a[i].toULongLong(&ok, 0);
            if (!ok)
                throw std::runtime_error("Invalid replay probe number");
            return n;
        };
        const Id event = number(2);
        const uint64_t count = number(3);
        if (number(4) > UINT32_MAX)
            throw std::runtime_error("Adapter vendor exceeds uint32");
        const auto vendor = uint32_t(number(4));
        QDir dir(a[5]);
        if (dir.exists() || !QDir().mkpath(a[5]))
            throw std::runtime_error("Probe directory must be new");
        auto save = [&](const QString &name, Bytes bytes) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                f.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                    qint64(bytes.size()))
                throw std::runtime_error("Cannot save geometry replay evidence");
        };
        QElapsedTimer timer;
        timer.start();
        const CancelCheck cancelled = [&] { return timer.elapsed() > 120000; };
        Frame frame(a[1].toStdWString());
        ReplayOptions o;
        o.debug = true;
        o.until = event;
        o.before = o.prepareBeforeDraw = true;
        o.warp = !vendor;
        if (vendor)
            o.vendor = vendor;
        Replay replay(frame, o);
        replay.run([&](Id, size_t, size_t) { checkCancellation(cancelled); });
        Id target = 0;
        replay.inspectNativeState([&](auto *context, const auto &objects) {
            Com<ID3D11RenderTargetView> view;
            context->OMGetRenderTargets(1, &view, nullptr);
            if (!view)
                throw std::runtime_error("Probe requires color target zero");
            Com<ID3D11Resource> resource;
            view->GetResource(&resource);
            for (const auto &[id, object] : objects) {
                Com<ID3D11Resource> candidate;
                if (SUCCEEDED(object.As(&candidate)) && candidate == resource) {
                    target = id;
                    break;
                }
            }
        });
        if (!target || textureInfo(frame.resource(target)).samples != 1)
            throw std::runtime_error("Probe requires an identified single-sample color target");
        const auto initial = replay.observeTextureStorage(target, 256ull * 1024 * 1024, cancelled).bytes;
        save("before.bin", initial);
        std::vector<uint8_t> actual;
        NativePixelGeometryRange range{0, count};
        range.timeoutMs = 60000;
        auto report = replayNativePixelGeometry(
            replay, event, range,
            [&](Replay &r, uint64_t completed) {
                if (completed == count)
                    actual = r.observeTextureStorage(target, 256ull * 1024 * 1024, cancelled).bytes;
            },
            cancelled);
        const auto restored = replay.observeTextureStorage(target, 256ull * 1024 * 1024, cancelled).bytes;
        save("private.bin", actual);
        save("restored.bin", restored);
        o.before = o.prepareBeforeDraw = false;
        o.measurement = ReplayOptions::Measurement{event, event, true};
        Replay reference(frame, o);
        reference.run([&](Id, size_t, size_t) { checkCancellation(cancelled); });
        const auto expected = reference.observeTextureStorage(target, 256ull * 1024 * 1024, cancelled).bytes;
        save("reference.bin", expected);
        if (!reference.measurementResult())
            throw std::runtime_error("Original draw measurement is missing");
        const auto referenceSamples = reference.measurementResult()->occlusion;
        uint64_t privateSamples = 0;
        for (const auto &batch : report.at("batches"))
            privateSamples += batch.at("passing_samples").get<uint64_t>();
        uint64_t differences = 0;
        for (size_t i = 0; i < std::max(actual.size(), expected.size()); ++i)
            if (i >= actual.size() || i >= expected.size() || actual[i] != expected[i])
                ++differences;
        const bool matched = !differences && initial == restored && privateSamples == referenceSamples;
        report["probe"] = {{"scope", "complete_primary_color_storage"},
                           {"target_resource", target},
                           {"before_sha256", sha256(initial)},
                           {"private_sha256", sha256(actual)},
                           {"restored_sha256", sha256(restored)},
                           {"reference_sha256", sha256(expected)},
                           {"differing_bytes", differences},
                           {"reference_passing_samples", referenceSamples},
                           {"query_matches", privateSamples == referenceSamples},
                           {"original_storage_restored", initial == restored},
                           {"elapsed_ms", timer.elapsed()},
                           {"status", matched ? "ok" : "different"}};
        const auto text = report.dump(2);
        save("report.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return matched ? 0 : 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
