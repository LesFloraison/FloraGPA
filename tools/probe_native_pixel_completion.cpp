#include "application/DxbcPixelCompletion.h"
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
        const auto args = app.arguments();
        if (args.size() != 9 && !(args.size() == 10 && args[9] == "--return-count") && args.size() != 11 &&
            !(args.size() == 12 && args[11] == "--presence"))
            throw std::runtime_error("Usage: FloraNativePixelCompletionProbe FRAME EVENT SLOT X Y SAMPLE "
                                     "VENDOR NEW_DIRECTORY [PRIMITIVE_MASK PRIMITIVE_VALUE [--presence] | "
                                     "--ordinal N | --return-count]");
        auto number = [&](int i) {
            bool ok = false;
            const auto value = args[i].toULongLong(&ok, 0);
            if (!ok || (i != 2 && value > UINT32_MAX))
                throw std::runtime_error("Invalid completion probe number");
            return value;
        };
        const Id event = number(2);
        const auto slot = uint32_t(number(3)), x = uint32_t(number(4)), y = uint32_t(number(5));
        const auto sample = uint32_t(number(6)), vendor = uint32_t(number(7));
        const bool returnCount = args.size() == 10 && args[9] == "--return-count";
        const auto ordinal = args.size() == 11 && args[9] == "--ordinal"
                                 ? std::optional<uint32_t>(uint32_t(number(10)))
                                 : std::nullopt;
        const std::optional<PixelPrimitiveMask> primitive =
            args.size() >= 11 && !ordinal
                ? std::optional<PixelPrimitiveMask>{{uint32_t(number(9)), uint32_t(number(10))}}
                : std::nullopt;
        if (slot >= 8 || sample)
            throw std::runtime_error("Completion probe currently selects non-MSAA sample zero");
        QDir directory(args[8]);
        if (directory.exists() || !QDir().mkpath(args[8]))
            throw std::runtime_error("Completion probe output directory must be new");
        auto save = [&](const QString &name, Bytes bytes) {
            QFile file(directory.filePath(name));
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                    qint64(bytes.size()))
                throw std::runtime_error("Cannot save completion evidence");
        };
        QElapsedTimer timer;
        timer.start();
        const CancelCheck cancel = [&] { return timer.elapsed() > 120000; };
        Frame frame(args[1].toStdWString());
        ReplayOptions options;
        options.debug = true;
        options.before = options.prepareBeforeDraw = true;
        options.until = event;
        options.warp = !vendor;
        if (vendor)
            options.vendor = vendor;
        Replay replay(frame, options);
        replay.run([&](Id, size_t, size_t) { checkCancellation(cancel); });
        Id resource = 0, shader = 0;
        replay.inspectNativeState([&](auto *context, const auto &objects) {
            std::array<ID3D11RenderTargetView *, 8> raw{};
            std::array<Com<ID3D11RenderTargetView>, 8> views;
            context->OMGetRenderTargets(8, raw.data(), nullptr);
            for (size_t i = 0; i < views.size(); ++i)
                views[i].Attach(raw[i]);
            if (!views[slot])
                throw std::runtime_error("Completion probe target is not bound");
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            views[slot]->GetDesc(&view);
            if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice)
                throw std::runtime_error("Completion probe requires a plain mip-zero Texture2D RTV");
            Com<ID3D11Resource> target;
            views[slot]->GetResource(&target);
            Com<ID3D11PixelShader> pixel;
            context->PSGetShader(&pixel, nullptr, nullptr);
            for (const auto &[id, object] : objects) {
                Com<ID3D11Resource> candidate;
                if (SUCCEEDED(object.As(&candidate)) && candidate == target)
                    resource = id;
                Com<ID3D11PixelShader> candidateShader;
                if (pixel && SUCCEEDED(object.As(&candidateShader)) && candidateShader == pixel)
                    shader = id;
            }
        });
        if (!resource || !shader)
            throw std::runtime_error("Completion probe requires identified target and shader");
        auto pixel = [&](Replay &r) {
            checkCancellation(cancel);
            auto bytes = r.readTexturePoint(resource, x, y, 0, 0, 0);
            checkCancellation(cancel);
            if (!bytes)
                throw std::runtime_error("Completion probe requires exact narrow storage readback");
            return *bytes;
        };
        const auto before = pixel(replay);
        std::vector<uint8_t> cleared, marked, original;
        const auto observer = [&](Replay &r, bool after) { (after ? marked : cleared) = pixel(r); };
        auto report = returnCount
                          ? replayNativePixelReturnCount(replay, event, {slot, x, y, 0, sample}, observer,
                                                         256ull * 1024 * 1024, 60000, cancel)
                      : ordinal ? replayNativePixelOrdinal(replay, event, {slot, *ordinal, true}, observer,
                                                           256ull * 1024 * 1024, 60000, cancel)
                      : args.size() == 12
                          ? replayNativePixelPrimitivePresence(replay, event, slot, *primitive, observer,
                                                               256ull * 1024 * 1024, 60000, cancel)
                      : primitive ? replayNativePixelPrimitiveMask(replay, event, slot, *primitive, observer,
                                                                   256ull * 1024 * 1024, 60000, cancel)
                                  : replayNativePixelCompletion(replay, event, slot, observer,
                                                                256ull * 1024 * 1024, 60000, cancel);
        const auto restored = pixel(replay);
        auto originalReport = replayNativeOriginalDraw(
            replay, event,
            [&](Replay &r, bool after) {
                if (after)
                    original = pixel(r);
            },
            256ull * 1024 * 1024, 60000, cancel);
        options.before = options.prepareBeforeDraw = false;
        options.measurement = ReplayOptions::Measurement{event, event, true};
        Replay reference(frame, options);
        reference.run([&](Id, size_t, size_t) { checkCancellation(cancel); });
        const auto expected = pixel(reference);
        const bool matched =
            before == restored && before == pixel(replay) && original == expected &&
            originalReport.at("passing_samples") == reference.measurementResult()->occlusion &&
            (ordinal || returnCount ||
             report.at("passing_samples") == originalReport.at("passing_samples")) &&
            (!returnCount || report.at("return_count").at("valid") == true);
        const auto source = frame.shader(frame.resource(shader).data);
        std::optional<PixelPrimitiveInput> producer;
        if (primitive && report.at("primitive_id_source") == "bound_geometry_shader")
            producer =
                PixelPrimitiveInput{report.at("completion_shader").at("input_register").get<uint32_t>(),
                                    report.at("completion_shader").at("input_component").get<uint32_t>()};
        auto rewritten =
            returnCount && report.at("count_transport").at("auxiliary_target") == true
                ? instrumentAuxiliaryPixelReturnCount(source,
                                                      report.at("count_transport").at("slot").get<uint32_t>(),
                                                      256ull * 1024 * 1024, cancel)
            : returnCount       ? instrumentPixelReturnCount(source, slot, 256ull * 1024 * 1024, cancel)
            : args.size() == 12 ? instrumentPixelPrimitivePresence(source, slot, *primitive,
                                                                   256ull * 1024 * 1024, cancel, producer)
            : primitive ? instrumentPixelPrimitiveMask(source, slot, *primitive, 256ull * 1024 * 1024, cancel,
                                                       producer)
                        : instrumentPixelCompletion(source, slot, {0x3f800000, 0x40000000, 0x40400000});
        if (rewritten.identity != report.at("completion_shader"))
            throw std::runtime_error("Completion probe shader identity differs");
        save("original.dxbc", source);
        save("marked.dxbc", rewritten.bytecode);
        save("before.bin", before);
        save("cleared.bin", cleared);
        save("marked.bin", marked);
        save("restored.bin", restored);
        save("original-draw.bin", original);
        save("reference.bin", expected);
        report["original_draw"] = originalReport;
        report["selection"] = {{"resource", resource}, {"shader", shader}, {"slot", slot}, {"x", x}, {"y", y},
                               {"sample", sample}};
        report["probe"] = {{"status", matched ? "ok" : "different"},
                           {"elapsed_ms", timer.elapsed()},
                           {"reference_passing_samples", reference.measurementResult()->occlusion},
                           {"marker_changed_storage", cleared != marked},
                           {"original_storage_restored", before == restored},
                           {"original_gpu_schedule", false}};
        if (returnCount) {
            report["probe"].erase("marker_changed_storage");
            report["probe"]["count_is_original_fragment_identity"] = false;
            report["probe"]["original_target_redirected"] =
                !report.at("count_transport").at("auxiliary_target").get<bool>();
        }
        if (primitive) {
            report["probe"].erase("marker_changed_storage");
            report["probe"]["diagnostic_storage_changed"] = cleared != marked;
            // Alpha can change even when the RGB predicate is false. This probe
            // retains exact storage without inventing a typed predicate decode.
            report["probe"]["primitive_predicate_matched"] = nullptr;
        }
        const auto text = report.dump(2);
        save("report.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        return matched ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
