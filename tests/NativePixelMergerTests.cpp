#include "BlendCapture.h"
#include "DepthStencilCapture.h"
#include "application/HlslCompilation.h"
#include "application/NativePixelMerger.h"
#include "application/NativePixelMergerSnapshot.h"
#include "application/NativePixelOutputs.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
Json depthDescriptor(const D3D11_DEPTH_STENCIL_DESC &d) {
    Json result = {uint32_t(d.DepthEnable), uint32_t(d.DepthWriteMask), uint32_t(d.DepthFunc),
                   uint32_t(d.StencilEnable),
                   uint32_t(d.StencilReadMask) | (uint32_t(d.StencilWriteMask) << 8)};
    for (const auto &face : {d.FrontFace, d.BackFace})
        for (const auto value : {uint32_t(face.StencilFailOp), uint32_t(face.StencilDepthFailOp),
                                 uint32_t(face.StencilPassOp), uint32_t(face.StencilFunc)})
            result.push_back(value);
    return result;
}
void save(const Json &result, const Json &outputs) {
    auto root = qEnvironmentVariable("FLORA_PIXEL_MERGER_EVIDENCE");
    if (root.isEmpty())
        return;
    const auto function = QString::fromLatin1(QTest::currentTestFunction());
    if (function == "msaa" || function == "coordinates" || function == "depthCoordinates" ||
        function == "depthOnly")
        root += "/" + function;
    QDir dir(root);
    if (!dir.exists() && !QDir().mkpath(root))
        throw std::runtime_error("Cannot create merger evidence directory");
    const auto text = Json{{"result", result}, {"quad_outputs", outputs}}.dump(2);
    QFile file(dir.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save merger evidence");
}
std::vector<uint8_t> storage(Replay &replay, Id resource, uint32_t format, uint32_t samples,
                             const char *phase = "before") {
    if (samples == 1)
        return replay.observeTextureStorage(resource, 1024 * 1024).bytes;
    std::vector<uint8_t> result;
    for (uint32_t sample = 0; sample < samples; ++sample) {
        std::vector<uint8_t> bytes;
        try {
            bytes = replay.readMsaa(resource, sample, format).bytes;
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Reference ") + phase + " sample " + std::to_string(sample) +
                                     " resource " + std::to_string(resource) + " read: " + e.what());
        }
        if (format == 20)
            for (size_t offset = 0; offset < bytes.size(); offset += 8)
                std::fill(bytes.begin() + offset + 5, bytes.begin() + offset + 8, 0);
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}
std::vector<uint8_t> selected(const std::vector<uint8_t> &storage, uint32_t format,
                              const NativePixelMergerState &state) {
    if (!format)
        return {};
    const auto texel = pitches(1, 1, format).first;
    const size_t plane = size_t(state.width) * state.height * texel;
    if (storage.size() != plane * state.samples.Count)
        throw std::runtime_error("Reference texture storage has unexpected dimensions");
    std::vector<uint8_t> result;
    for (uint32_t sample = 0; sample < state.samples.Count; ++sample) {
        const auto offset = sample * plane + (size_t(state.y) * state.width + state.x) * texel;
        result.insert(result.end(), storage.begin() + offset, storage.begin() + offset + texel);
    }
    return result;
}
Json run(testing::Capture &capture, uint32_t vendor, Id event, Id color, Id depth,
         NativePixelMergerState &state, ReplayOptions options = {}) {
    QTemporaryDir dir;
    const auto path = dir.path() + "/merger.gpa_frame";
    capture.save(path);
    Frame frame(path.toStdWString());
    options.until = event;
    options.warp = !vendor;
    options.vendor = vendor ? std::optional(vendor) : std::nullopt;
    options.debug = true;
    options.blendEdits[event] = {state.blend, state.blendFactor, state.sampleMask & state.coverageMask};
    options.depthStencilEdits[event] = {state.depthStencil, state.stencilRef};
    options.before = true;
    Replay before(frame, options);
    const auto snapshot = snapshotResourceInputs(before, 4);
    const auto outputs = evaluateNativePixelOutputs(snapshot, {});
    const auto originalColor =
        color ? storage(before, color, state.colorFormat, state.samples.Count) : std::vector<uint8_t>();
    const auto originalDepth =
        depth ? storage(before, depth, state.depthFormat, state.samples.Count) : std::vector<uint8_t>();
    state.color = selected(originalColor, state.colorFormat, state);
    state.depth = selected(originalDepth, state.depthFormat, state);
    NativePixelMergerSelection selection;
    if (color)
        selection.colorSlot = state.colorSlot;
    selection.x = state.x;
    selection.y = state.y;
    auto observed = snapshotNativePixelMergerState(before, selection);
    // Query an independently created state without relying on the snapshot or
    // context binding. Disabled fields can be canonicalized by D3D11 creation.
    Com<ID3D11DepthStencilState> referenceDepthState;
    check(before.nativeDevice()->CreateDepthStencilState(&state.depthStencil, &referenceDepthState),
          "Create independent depth descriptor reference");
    D3D11_DEPTH_STENCIL_DESC referenceDepth{};
    referenceDepthState->GetDesc(&referenceDepth);
    if (depthDescriptor(observed.state.depthStencil) != depthDescriptor(referenceDepth))
        throw std::runtime_error("Snapshot depth descriptor differs from independent D3D11 state");
    if (observed.state.color != state.color || observed.state.depth != state.depth ||
        observed.state.colorFormat != state.colorFormat || observed.state.depthFormat != state.depthFormat ||
        observed.state.width != state.width || observed.state.height != state.height ||
        observed.state.samples.Count != state.samples.Count ||
        observed.state.samples.Quality != state.samples.Quality ||
        observed.state.depthViewFlags != state.depthViewFlags ||
        observed.state.sampleMask != (state.sampleMask & state.coverageMask))
        throw std::runtime_error("Observed merger bindings differ from independent fixture state");
    // Fixtures model geometric coverage using the application's sample mask.
    // Restore that known factorization only after checking the observed mask;
    // actual blend/depth descriptors and storage come entirely from the snapshot.
    observed.state.sampleMask = state.sampleMask;
    observed.state.coverageMask = state.coverageMask;
    observed.state.rasterDepthBits = state.rasterDepthBits;
    observed.state.frontFace = state.frontFace;
    auto result = mergeNativePixelOutput(before.nativeDevice(), options.warp, observed.state, outputs, 0);
    result["binding_snapshot"] = observed.identity;
    result["fixture_depth_stencil_descriptor"] = depthDescriptor(state.depthStencil);
    result["reference_depth_stencil_descriptor"] = depthDescriptor(referenceDepth);
    if ((color && storage(before, color, state.colorFormat, state.samples.Count, "after diagnostic") !=
                      originalColor) ||
        (depth &&
         storage(before, depth, state.depthFormat, state.samples.Count, "after diagnostic") != originalDepth))
        throw std::runtime_error("Private merger changed reference storage");
    options.before = false;
    if (!color)
        options.measurement = ReplayOptions::Measurement{event, event, true};
    Replay after(frame, options);
    after.run();
    if (!color) {
        if (!after.measurementResult())
            throw std::runtime_error("Missing original depth-only draw measurement");
        result["reference_entire_draw_passing_samples"] = after.measurementResult()->occlusion;
    }
    const auto expectedColor =
        color ? selected(storage(after, color, state.colorFormat, state.samples.Count, "after"),
                         state.colorFormat, state)
              : std::vector<uint8_t>();
    const auto expectedDepth =
        depth ? selected(storage(after, depth, state.depthFormat, state.samples.Count, "after"),
                         state.depthFormat, state)
              : std::vector<uint8_t>();
    result["reference_color"] = expectedColor;
    result["reference_depth"] = expectedDepth;
    result["reference_storage_unchanged"] = true;
    if (result.at("color_after") != Json(expectedColor) || result.at("depth_after") != Json(expectedDepth)) {
        save(result, outputs);
        throw std::runtime_error("Private merger differs from original captured GPU draw: " + result.dump());
    }
    save(result, outputs);
    return result;
}
} // namespace
class NativePixelMergerTests final : public QObject {
    Q_OBJECT
  private slots:
    void snapshotSelection_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"array1d", "array2d", "volume", "msaa"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(mode))) << vendor << QString(mode);
    }
    void snapshotSelection() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            const bool volume = mode == "volume", one = mode == "array1d", msaa = mode == "msaa";
            auto capture = testing::blendCapture();
            std::erase_if(capture.entries, [](const auto &entry) {
                return entry.id == 1000 || entry.id == 1001 || entry.id == 1002 || entry.id == 90;
            });
            Resource resource;
            resource.type = one ? 0x84 : volume ? 0x86 : 0x85;
            resource.desc =
                one      ? std::vector<uint32_t>{8, 2, 3, 3, 0, 32, 0, 0}
                : volume ? std::vector<uint32_t>{8, 4, 4, 2, 3, 0, 32, 0, 0}
                         : std::vector<uint32_t>{8, 4, msaa ? 1u : 2u, 3, 3, msaa ? 4u : 1u, 0, 0, 32, 0, 0};
            auto payload = testing::statePack(Id(0), Id(0));
            for (auto word : resource.desc)
                testing::append(payload, word);
            testing::append(payload, msaa ? Id(0) : Id(1001));
            capture.add(1000, 5, resource.type, payload);
            if (!msaa) {
                std::vector<uint8_t> bytes;
                for (const auto &sub : textureSubresources(resource))
                    for (uint32_t z = 0; z < sub.depth; ++z)
                        for (uint32_t y = 0; y < sub.height; ++y)
                            for (uint32_t x = 0; x < sub.width; ++x) {
                                const auto tag = x + 16 * y + 64 * z + 256 * sub.layer + 4096 * sub.mip;
                                for (uint32_t value : {tag, tag ^ 0x80000000u, tag + 5, 0xdeadbeefu})
                                    testing::append(bytes, value);
                            }
                auto data = testing::word(uint32_t(bytes.size()));
                data.insert(data.end(), bytes.begin(), bytes.end());
                capture.add(1001, 9, 1, data);
            }
            capture.add(1002, 5, 0x8d,
                        testing::statePack(Id(0), Id(0), Id(1000), 3u,
                                           one      ? 3u
                                           : volume ? 8u
                                           : msaa   ? 7u
                                                    : 5u,
                                           1u,
                                           msaa     ? 2u
                                           : volume ? 0u
                                                    : 1u,
                                           msaa ? 0u : 2u));
            if (msaa)
                capture.add(70, 7, 0x32,
                            testing::statePack(Id(0), Id(1), Id(1002), uint8_t(1), 1.f, 2.f, 3.f, 4.f));
            State bound{};
            bound.topology = 4;
            bound.sampleMask = 0x55;
            bound.blendFactor = {.125f, .25f, .5f, 1.f};
            bound.blend = 64;
            bound.omStart = 8;
            bound.rtCount = 3;
            bound.rtv[2] = 1002;
            bound.rasterizer = 52;
            bound.viewports = 50;
            bound.stages[0].shader = 10;
            bound.stages[4].shader = 12;
            capture.add(90, 3, 3, testing::snapshot(bound));
            QTemporaryDir dir;
            const auto path = dir.filePath("snapshot.gpa_frame");
            capture.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            options.debug = options.before = options.prepareBeforeDraw = true;
            options.until = 100;
            Replay replay(frame, options);
            replay.run();
            NativePixelMergerSelection selection;
            selection.colorSlot = 2;
            selection.x = msaa ? 7 : 3;
            selection.y = one ? 0 : msaa ? 3 : 1;
            selection.layer = 1;
            auto observed = snapshotNativePixelMergerState(replay, selection);
            const auto tag = selection.x + 16 * selection.y + (volume ? 64 : 512) + 4096;
            std::vector<uint8_t> expected;
            for (unsigned sample = 0; sample < (msaa ? 4u : 1u); ++sample)
                for (uint32_t value :
                     msaa ? std::array<uint32_t, 4>{1, 2, 3, 4}
                          : std::array<uint32_t, 4>{tag, tag ^ 0x80000000u, tag + 5, 0xdeadbeefu})
                    testing::append(expected, value);
            QCOMPARE(observed.state.color, expected);
            QCOMPARE(observed.state.colorSlot, 2u);
            QCOMPARE(observed.state.sampleMask, 0x55u);
            QCOMPARE(observed.state.blendFactor, bound.blendFactor);
            QVERIFY(!observed.state.rasterDepthBits);
            QCOMPARE(observed.identity["targets"][0]["subresource"]["layer"], Json(volume ? 1 : 2));
            selection.maxReadbackBytes = observed.identity.at("readback_bytes").get<uint64_t>();
            QCOMPARE(snapshotNativePixelMergerState(replay, selection).state.color, expected);
            auto bad = selection;
            --bad.maxReadbackBytes;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, snapshotNativePixelMergerState(replay, bad));
            bad = selection;
            bad.layer = 2;
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, snapshotNativePixelMergerState(replay, bad));
            bad = selection;
            bad.colorSlot = 1;
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, snapshotNativePixelMergerState(replay, bad));
            bad.colorSlot.reset();
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, snapshotNativePixelMergerState(replay, bad));
            unsigned checks = 0;
            QVERIFY_THROWS_EXCEPTION(
                OperationCancelled,
                snapshotNativePixelMergerState(replay, selection, [&] { return ++checks > 6; }));
            QCOMPARE(snapshotNativePixelMergerState(replay, selection).state.color, expected);
            const auto root = qEnvironmentVariable("FLORA_PIXEL_MERGER_EVIDENCE");
            if (!root.isEmpty()) {
                const auto folder = root + "/snapshotSelection";
                if (!QDir().mkpath(folder))
                    throw std::runtime_error("Cannot create snapshot selection evidence");
                QFile file(QDir(folder).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
                auto report = observed.identity;
                report.update({{"color", observed.state.color},
                               {"expected_color", expected},
                               {"vendor_request", vendor},
                               {"mode", mode.toStdString()},
                               {"cancellation_checks", checks},
                               {"retry_verified", true}});
                const auto text = report.dump(2);
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                    file.write(text.data(), qint64(text.size())) != qint64(text.size()))
                    throw std::runtime_error("Cannot save snapshot selection evidence");
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void depthOnly_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 2u, 4u})
                for (uint32_t format : {20u, 40u, 45u, 55u})
                    for (const auto *mode :
                         {"raster", "shader-depth", "discard", "sample-mask", "coverage", "alpha",
                          "no-depth-write", "read-only-depth", "depth-fail", "stencil-pass", "stencil-fail",
                          "stencil-back", "read-only-stencil", "unchanged"}) {
                        if (QString(mode).contains("stencil") && format != 20 && format != 45)
                            continue;
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(samples).arg(format).arg(mode)))
                            << vendor << samples << format << QString(mode);
                    }
    }
    void depthOnly() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, samples);
            QFETCH(uint32_t, format);
            QFETCH(QString, mode);
            NativePixelMergerState state;
            state.width = 7;
            state.height = 5;
            state.x = 6;
            state.y = 4;
            state.samples.Count = samples;
            state.depthFormat = format;
            state.rasterDepthBits = 0;
            state.frontFace = mode != "stencil-back";
            state.stencilRef = 0xa5;
            state.depthStencil.StencilEnable = format == 20 || format == 45;
            if (mode == "unchanged") {
                state.rasterDepthBits = std::bit_cast<uint32_t>(.5f);
                state.depthStencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
                state.depthStencil.StencilEnable = FALSE;
            }
            auto &face = state.frontFace ? state.depthStencil.FrontFace : state.depthStencil.BackFace;
            face.StencilPassOp = mode == "stencil-back" ? D3D11_STENCIL_OP_INVERT : D3D11_STENCIL_OP_REPLACE;
            if (mode == "stencil-fail") {
                face.StencilFunc = D3D11_COMPARISON_NEVER;
                face.StencilFailOp = D3D11_STENCIL_OP_REPLACE;
            }
            if (mode == "depth-fail") {
                state.depthStencil.DepthFunc = D3D11_COMPARISON_NEVER;
                face.StencilDepthFailOp = D3D11_STENCIL_OP_INVERT;
            }
            if (mode == "no-depth-write")
                state.depthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            state.depthViewFlags = mode == "read-only-depth"     ? D3D11_DSV_READ_ONLY_DEPTH
                                   : mode == "read-only-stencil" ? D3D11_DSV_READ_ONLY_STENCIL
                                                                 : 0;
            if (mode == "sample-mask")
                state.sampleMask = 0x55555555;
            state.blend.AlphaToCoverageEnable = mode == "alpha";
            auto capture = testing::depthStencilCapture(!state.frontFace);
            for (const auto &entry : capture.entries) {
                if (entry.id == 720) {
                    testing::put(capture.bytes, size_t(entry.offset) + 16, state.width);
                    testing::put(capture.bytes, size_t(entry.offset) + 20, state.height);
                    testing::put(capture.bytes, size_t(entry.offset) + 32, format);
                    testing::put(capture.bytes, size_t(entry.offset) + 36, samples);
                }
                if (entry.id == 22) {
                    testing::put(capture.bytes, size_t(entry.offset) + 12, float(state.width));
                    testing::put(capture.bytes, size_t(entry.offset) + 16, float(state.height));
                }
                if (entry.id == 722) {
                    testing::put(capture.bytes, size_t(entry.offset) + 24, format);
                    testing::put(capture.bytes, size_t(entry.offset) + 28, samples > 1 ? 5u : 3u);
                    testing::put(capture.bytes, size_t(entry.offset) + 32, state.depthViewFlags);
                }
                if (entry.id == 724)
                    testing::put(capture.bytes, size_t(entry.offset) + 48, samples > 1 ? 1u : 0u);
                if (entry.id == 810) {
                    testing::put(capture.bytes, size_t(entry.offset) + 16, Id(723));
                    if (format != 20 && format != 45)
                        testing::put(capture.bytes, size_t(entry.offset) + 24, 1u);
                }
            }
            capture.add(
                723, 5, 0x8e,
                testing::statePack(Id(0), Id(0), Id(720), format, samples > 1 ? 5u : 3u, 0u, 0u, 0u, 0u));
            std::erase_if(capture.entries,
                          [](const auto &entry) { return entry.id == 990 || entry.id == 1990; });
            State bound{};
            bound.topology = 4;
            bound.sampleMask = UINT32_MAX;
            bound.omStart = 8;
            bound.dsv = 722;
            bound.rasterizer = 724;
            bound.viewports = 22;
            bound.stages[0].shader = 30;
            bound.stages[4].shader = 32;
            capture.add(990, 3, 3, testing::snapshot(bound));
            capture.add(1990, 3, 3, testing::snapshot(bound));
            ReplayOptions options;
            const char *source = mode == "shader-depth" ? "float main():SV_Depth{return .25;}"
                                 : mode == "discard"    ? "void main(){discard;}"
                                 : mode == "coverage"   ? "uint main():SV_Coverage{return 5;}"
                                 : mode == "alpha" ? "float4 main():SV_Target{return float4(0,0,0,.375);}"
                                                   : "void main(){}";
            options.shaders[32] = compileHlsl(source, "ps_5_0").bytecode;
            if (mode == "unchanged")
                options.shaders[30] =
                    compileHlsl(
                        "float4 main(uint id:SV_VertexID):SV_Position{"
                        "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),.5,1);}",
                        "vs_5_0")
                        .bytecode;
            const auto result = run(capture, vendor, 1000, 0, 720, state, options);
            QCOMPARE(result.at("color_format"), Json(0));
            QCOMPARE(result.at("color_before"), Json::array());
            QCOMPARE(result.at("color_after"), Json::array());
            QVERIFY(result.at("original_fragment_eligibility").is_null());
            if (mode == "discard" || mode == "stencil-fail" || mode == "depth-fail")
                QCOMPARE(result.at("passing_samples"), Json(0));
            if (mode == "unchanged") {
                QCOMPARE(result.at("depth_before"), result.at("depth_after"));
                QCOMPARE(result.at("passing_samples"), Json(samples));
                QCOMPARE(result.at("reference_entire_draw_passing_samples"),
                         Json(state.width * state.height * samples));
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void blend_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint32_t>("value");
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            const auto row = [&](const char *mode, uint32_t value) {
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(mode).arg(value)))
                    << vendor << QString(mode) << value;
            };
            for (uint32_t factor :
                 {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 14u, 15u, 16u, 17u, 18u, 19u}) {
                row("source", factor);
                row("destination", factor);
            }
            for (uint32_t op = 1; op <= 5; ++op)
                row("operation", op);
            for (uint32_t op = 0; op < 16; ++op)
                row("logic", op);
            for (uint32_t mask : {0u, 1u, 5u, 10u, 15u})
                row("write-mask", mask);
            for (uint32_t slot : {3u, 7u})
                row("slot", slot);
            for (uint32_t mask : {0u, 1u, 2u})
                row("sample-mask", mask);
            for (uint32_t mode = 0; mode < 4; ++mode)
                row("shader", mode);
            for (uint32_t format : {10u, 11u, 24u, 28u, 29u, 41u, 49u, 54u, 61u, 87u, 91u})
                row("format", format);
        }
    }
    void blend() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            QFETCH(uint32_t, value);
            const bool dual = (mode == "source" || mode == "destination") && value >= 16;
            const uint32_t slot = mode == "slot" ? value : 0;
            auto capture = testing::blendCapture(slot + 1, dual, mode == "logic");
            NativePixelMergerState state;
            state.colorFormat = mode == "logic" ? 3 : 2;
            if (mode == "format") {
                state.colorFormat = value;
                for (const auto &entry : capture.entries) {
                    if (entry.id == 1000)
                        testing::put(capture.bytes, size_t(entry.offset) + 32, value);
                    if (entry.id == 1002)
                        testing::put(capture.bytes, size_t(entry.offset) + 24, value);
                }
                std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 1001; });
                auto data = testing::word(pitches(1, 1, value).first);
                data.resize(data.size() + pitches(1, 1, value).first);
                capture.add(1001, 9, 1, data);
            }
            state.colorSlot = slot;
            state.blendFactor = {.25f, .5f, .75f, 1.f};
            auto &blend = state.blend.RenderTarget[slot];
            state.blend.IndependentBlendEnable = slot != 0;
            blend.BlendEnable = mode == "source" || mode == "destination" || mode == "operation";
            if (mode == "source" || mode == "destination") {
                blend.SrcBlend = D3D11_BLEND(mode == "source" ? value : 1);
                blend.DestBlend = D3D11_BLEND(mode == "destination" ? value : 1);
            }
            if (mode == "operation") {
                blend.SrcBlend = blend.DestBlend = D3D11_BLEND_ONE;
                blend.BlendOp = blend.BlendOpAlpha = D3D11_BLEND_OP(value);
            }
            if (mode == "logic") {
                blend.LogicOpEnable = TRUE;
                blend.LogicOp = D3D11_LOGIC_OP(value);
            }
            if (mode == "write-mask")
                blend.RenderTargetWriteMask = uint8_t(value);
            if (mode == "sample-mask")
                state.sampleMask = value;
            ReplayOptions options;
            if (mode == "shader") {
                state.blend.AlphaToCoverageEnable = value >= 2;
                const auto code = value == 0
                                      ? "float4 main():SV_Target{discard;return float4(.5,.25,.75,.5);}"
                                  : value == 1 ? "struct O{float4 c:SV_Target;uint mask:SV_Coverage;};O "
                                                 "main(){O o;o.c=float4(.5,.25,.75,.5);o.mask=0;return o;}"
                                  : value == 2 ? "float4 main():SV_Target{return float4(.5,.25,.75,0);}"
                                               : "struct O{float4 c:SV_Target;uint mask:SV_Coverage;};O "
                                                 "main(){O o;o.c=float4(.5,.25,.75,0);o.mask=1;return o;}";
                options.shaders[12] = compileHlsl(code, "ps_5_0").bytecode;
            }
            const auto result = run(capture, vendor, 100, 1000 + 4 * slot, 0, state, options);
            QCOMPARE(result.at("scope"), Json("private_single_sample_output_merger"));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void depth_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("front");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint32_t>("value");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool front : {true, false}) {
                const auto row = [&](const char *mode, uint32_t value) {
                    QTest::newRow(
                        qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(front).arg(mode).arg(value)))
                        << vendor << front << QString(mode) << value;
                };
                for (uint32_t value = 1; value <= 8; ++value)
                    for (const auto *mode : {"fail", "depth-fail", "pass", "depth-func", "stencil-func"})
                        row(mode, value);
                row("write-mask", 0x0f);
                row("read-mask", 0x0f);
                row("no-depth-write", 0);
                for (uint32_t flags : {1u, 2u, 3u})
                    row("read-only", flags);
                row("format", 40);
                row("format", 55);
                for (uint32_t value = 0; value < 3; ++value)
                    row("shader-depth", value);
            }
    }
    void depth() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(bool, front);
            QFETCH(QString, mode);
            QFETCH(uint32_t, value);
            auto capture = testing::depthStencilCapture(!front);
            NativePixelMergerState state;
            state.colorFormat = 28;
            state.depthFormat = 45;
            state.rasterDepthBits = 0;
            state.frontFace = front;
            state.stencilRef = 0xa5;
            state.depthStencil.StencilEnable = TRUE;
            if (mode == "read-only") {
                state.depthViewFlags = value;
                for (const auto &entry : capture.entries) {
                    if (entry.id == 722)
                        testing::put(capture.bytes, size_t(entry.offset) + 32, value);
                    if (entry.id == 810)
                        testing::put(capture.bytes, size_t(entry.offset) + 16, Id(723));
                }
                capture.add(723, 5, 0x8e, testing::statePack(Id(0), Id(0), Id(720), 45u, 3u, 0u, 0u, 0u, 0u));
            } else if (mode == "format") {
                state.depthFormat = value;
                state.depthStencil.StencilEnable = FALSE;
                for (const auto &entry : capture.entries) {
                    if (entry.id == 720)
                        testing::put(capture.bytes, size_t(entry.offset) + 32, value);
                    if (entry.id == 722)
                        testing::put(capture.bytes, size_t(entry.offset) + 24, value);
                    if (entry.id == 810)
                        testing::put(capture.bytes, size_t(entry.offset) + 24, 1u);
                }
            }
            auto &face = front ? state.depthStencil.FrontFace : state.depthStencil.BackFace;
            if (mode == "fail") {
                face.StencilFunc = D3D11_COMPARISON_NEVER;
                face.StencilFailOp = D3D11_STENCIL_OP(value);
            } else if (mode == "depth-fail") {
                state.depthStencil.DepthFunc = D3D11_COMPARISON_NEVER;
                face.StencilDepthFailOp = D3D11_STENCIL_OP(value);
            } else if (mode == "pass")
                face.StencilPassOp = D3D11_STENCIL_OP(value);
            else if (mode == "read-only")
                face.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            else if (mode == "depth-func")
                state.depthStencil.DepthFunc = D3D11_COMPARISON_FUNC(value);
            else if (mode == "stencil-func")
                face.StencilFunc = D3D11_COMPARISON_FUNC(value);
            else if (mode == "write-mask") {
                face.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
                state.depthStencil.StencilWriteMask = uint8_t(value);
            } else if (mode == "read-mask") {
                state.stencilRef = 3;
                state.depthStencil.StencilReadMask = uint8_t(value);
                face.StencilFunc = D3D11_COMPARISON_EQUAL;
                face.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            } else if (mode == "no-depth-write")
                state.depthStencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            ReplayOptions options;
            if (mode == "shader-depth") {
                state.rasterDepthBits = std::bit_cast<uint32_t>(.5f);
                options.shaders[30] = compileHlsl("float4 main(uint v:SV_VertexID):SV_Position{return "
                                                  "float4(v==2?3:-1,v==1?3:-1,.5,1);}",
                                                  "vs_5_0")
                                          .bytecode;
                const auto semantic = value == 0   ? "SV_Depth"
                                      : value == 1 ? "SV_DepthGreaterEqual"
                                                   : "SV_DepthLessEqual";
                options.shaders[32] =
                    compileHlsl("struct O{float4 color:SV_Target;float depth:" + std::string(semantic) +
                                    ";};O main(){O o;o.color=float4(1,0,0,1);o.depth=" +
                                    (value == 2 ? ".25" : ".75") + ";return o;}",
                                "ps_5_0")
                        .bytecode;
            }
            run(capture, vendor, 1000, 20, 720, state, options);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void msaa_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint32_t>("sample");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {2u, 4u})
                for (const auto *name : {"blend",
                                         "write-mask",
                                         "coverage",
                                         "sample-mask",
                                         "unchanged",
                                         "dual",
                                         "alpha",
                                         "coverage-output",
                                         "discard",
                                         "sequence",
                                         "depth",
                                         "depth-fail",
                                         "stencil",
                                         "read-only-depth",
                                         "read-only-stencil",
                                         "d16",
                                         "d32",
                                         "d32s8",
                                         "format-half",
                                         "format-srgb",
                                         "format-bgra",
                                         "format-inherit"})
                    for (uint32_t sample = 0; sample < (std::string(name) == "coverage" ? samples : 1);
                         ++sample)
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(samples).arg(name).arg(sample)))
                            << vendor << samples << QString(name) << sample;
    }
    void msaa() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, samples);
            QFETCH(QString, mode);
            QFETCH(uint32_t, sample);
            const bool depth = mode == "depth" || mode == "depth-fail" || mode == "stencil" ||
                               mode.startsWith("read-only") || mode == "d16" || mode == "d32" ||
                               mode == "d32s8";
            auto capture = depth ? testing::depthStencilCapture()
                                 : testing::blendCapture(1, mode == "dual", false, samples);
            NativePixelMergerState state;
            state.samples.Count = samples;
            state.colorFormat = depth                   ? 28
                                : mode == "format-half" ? 10
                                : mode == "format-srgb" ? 29
                                : mode == "format-bgra" ? 87
                                                        : 2;
            state.coverageMask = mode == "coverage" ? 1u << sample : UINT32_MAX;
            state.sampleMask = mode == "sample-mask" ? 0 : mode == "sequence" ? 2 : 0x55555555;
            state.blend.AlphaToCoverageEnable = mode == "alpha" || mode == "coverage-output";
            auto &blend = state.blend.RenderTarget[0];
            if (mode == "blend" || mode == "dual") {
                blend.BlendEnable = TRUE;
                blend.SrcBlend = mode == "dual" ? D3D11_BLEND_SRC1_COLOR : D3D11_BLEND_SRC_ALPHA;
                blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            }
            if (mode == "write-mask")
                blend.RenderTargetWriteMask = 5;
            if (mode == "unchanged")
                blend.RenderTargetWriteMask = 0;
            ReplayOptions options;
            if (mode == "sequence")
                options.blendEdits[100] = {state.blend, state.blendFactor, 1};
            if (mode == "coverage-output" || mode == "discard")
                options.shaders[12] =
                    compileHlsl(mode == "discard"
                                    ? "float4 main():SV_Target{discard;return float4(.5,.25,.75,.5);}"
                                    : "struct O{float4 c:SV_Target;uint mask:SV_Coverage;};O main(){O "
                                      "o;o.c=float4(.5,.25,.75,0);o.mask=3;return o;}",
                                "ps_5_0")
                        .bytecode;
            if (depth) {
                state.depthFormat = mode == "d16" ? 55 : mode == "d32" ? 40 : mode == "d32s8" ? 20 : 45;
                state.rasterDepthBits = 0;
                state.stencilRef = 0xa5;
                state.depthStencil.StencilEnable =
                    mode == "stencil" || mode.startsWith("read-only") || mode == "d32s8";
                state.depthStencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
                if (mode == "depth-fail")
                    state.depthStencil.DepthFunc = D3D11_COMPARISON_NEVER;
                state.depthViewFlags = mode == "read-only-depth" ? 1 : mode == "read-only-stencil" ? 2 : 0;
                for (const auto &entry : capture.entries) {
                    if (entry.id == 20 || entry.id == 720)
                        testing::put(capture.bytes, size_t(entry.offset) + 36, samples);
                    if (entry.id == 720)
                        testing::put(capture.bytes, size_t(entry.offset) + 32, state.depthFormat);
                    if (entry.id == 21)
                        testing::put(capture.bytes, size_t(entry.offset) + 28, 6u);
                    if (entry.id == 722) {
                        testing::put(capture.bytes, size_t(entry.offset) + 24, state.depthFormat);
                        testing::put(capture.bytes, size_t(entry.offset) + 28, 5u);
                        testing::put(capture.bytes, size_t(entry.offset) + 32, state.depthViewFlags);
                    }
                    if (entry.id == 724)
                        testing::put(capture.bytes, size_t(entry.offset) + 48, 1u);
                    if (entry.id == 810) {
                        testing::put(capture.bytes, size_t(entry.offset) + 16, Id(723));
                        if (mode == "d16" || mode == "d32")
                            testing::put(capture.bytes, size_t(entry.offset) + 24, 1u);
                    }
                }
                capture.add(723, 5, 0x8e,
                            testing::statePack(Id(0), Id(0), Id(720), state.depthFormat, 5u, 0u, 0u, 0u, 0u));
            } else if (mode.startsWith("format")) {
                for (const auto &entry : capture.entries) {
                    if (entry.id == 1000)
                        testing::put(capture.bytes, size_t(entry.offset) + 32, state.colorFormat);
                    if (entry.id == 1002)
                        testing::put(capture.bytes, size_t(entry.offset) + 24,
                                     mode == "format-inherit" ? 0u : state.colorFormat);
                }
            }
            const auto result = run(capture, vendor,
                                    depth                ? 1000
                                    : mode == "sequence" ? 200
                                                         : 100,
                                    depth ? 20 : 1000, depth ? 720 : 0, state, options);
            QCOMPARE(result.at("scope"), Json("private_multisample_output_merger"));
            QCOMPARE(result.at("sample_count"), Json(samples));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void coordinates_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("position");
        QTest::addColumn<uint32_t>("alpha");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 2u, 4u})
                for (uint32_t position = 0; position < 4; ++position)
                    for (uint32_t alpha : {1u, 3u, 5u, 7u})
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(samples).arg(position).arg(alpha)))
                            << vendor << samples << position << alpha;
    }
    void coordinates() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, samples);
            QFETCH(uint32_t, position);
            QFETCH(uint32_t, alpha);
            auto capture = testing::blendCapture(1, false, false, samples);
            NativePixelMergerState state;
            state.width = 7;
            state.height = 5;
            state.x = std::array{0u, 1u, 4u, 6u}[position];
            state.y = std::array{0u, 1u, 2u, 4u}[position];
            state.colorFormat = 2;
            state.samples.Count = samples;
            state.blend.AlphaToCoverageEnable = TRUE;
            for (const auto &entry : capture.entries) {
                if (entry.id == 1000) {
                    testing::put(capture.bytes, size_t(entry.offset) + 16, state.width);
                    testing::put(capture.bytes, size_t(entry.offset) + 20, state.height);
                }
                if (entry.id == 50) {
                    testing::put(capture.bytes, size_t(entry.offset) + 12, float(state.width));
                    testing::put(capture.bytes, size_t(entry.offset) + 16, float(state.height));
                }
            }
            if (samples == 1) {
                std::erase_if(capture.entries, [](const auto &entry) { return entry.id == 1001; });
                auto data = testing::word(state.width * state.height * 16);
                for (uint32_t pixel = 0; pixel < state.width * state.height; ++pixel)
                    for (float value : {.125f, .5f, .25f, .25f})
                        testing::append(data, value);
                capture.add(1001, 9, 1, data);
            }
            ReplayOptions options;
            options.shaders[12] = compileHlsl("float4 main():SV_Target{return float4(.5,.25,.75," +
                                                  std::to_string(float(alpha) / 8) + ");}",
                                              "ps_5_0")
                                      .bytecode;
            const auto result = run(capture, vendor, 100, 1000, 0, state, options);
            QCOMPARE(result.at("private_pixel_location"), Json({state.x, state.y}));
            QCOMPARE(result.at("private_target_extent"), Json({7, 5}));
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void depthCoordinates_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("position");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 2u, 4u})
                for (uint32_t format : {20u, 40u, 45u, 55u})
                    for (uint32_t position = 0; position < 2; ++position)
                        QTest::newRow(qPrintable(
                            QString("%1-%2-%3-%4").arg(vendor).arg(samples).arg(format).arg(position)))
                            << vendor << samples << format << position;
    }
    void depthCoordinates() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, samples);
            QFETCH(uint32_t, format);
            QFETCH(uint32_t, position);
            auto capture = testing::depthStencilCapture();
            NativePixelMergerState state;
            state.width = 7;
            state.height = 5;
            state.x = position ? 6 : 1;
            state.y = position ? 4 : 1;
            state.colorFormat = 28;
            state.depthFormat = format;
            state.samples.Count = samples;
            state.sampleMask = 0x55555555;
            state.rasterDepthBits = 0;
            state.stencilRef = 0xa5;
            state.depthStencil.StencilEnable = format == 20 || format == 45;
            state.depthStencil.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            for (const auto &entry : capture.entries) {
                if (entry.id == 20 || entry.id == 720) {
                    testing::put(capture.bytes, size_t(entry.offset) + 16, state.width);
                    testing::put(capture.bytes, size_t(entry.offset) + 20, state.height);
                    testing::put(capture.bytes, size_t(entry.offset) + 36, samples);
                }
                if (entry.id == 22) {
                    testing::put(capture.bytes, size_t(entry.offset) + 12, float(state.width));
                    testing::put(capture.bytes, size_t(entry.offset) + 16, float(state.height));
                }
                if (entry.id == 720)
                    testing::put(capture.bytes, size_t(entry.offset) + 32, format);
                if (entry.id == 21)
                    testing::put(capture.bytes, size_t(entry.offset) + 28, samples > 1 ? 6u : 4u);
                if (entry.id == 722) {
                    testing::put(capture.bytes, size_t(entry.offset) + 24, format);
                    testing::put(capture.bytes, size_t(entry.offset) + 28, samples > 1 ? 5u : 3u);
                }
                if (entry.id == 724)
                    testing::put(capture.bytes, size_t(entry.offset) + 48, samples > 1 ? 1u : 0u);
                if (entry.id == 810 && !state.depthStencil.StencilEnable)
                    testing::put(capture.bytes, size_t(entry.offset) + 24, 1u);
            }
            run(capture, vendor, 1000, 20, 720, state);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void boundaries() {
        auto capture = testing::blendCapture();
        QTemporaryDir dir;
        const auto path = dir.path() + "/boundary.gpa_frame";
        capture.save(path);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = options.before = options.debug = true;
        options.until = 100;
        Replay replay(frame, options);
        const auto snapshot = snapshotResourceInputs(replay, 4);
        const auto outputs = evaluateNativePixelOutputs(snapshot, {});
        NativePixelMergerState s;
        s.colorFormat = 2;
        s.color = replay.observeTextureStorage(1000, 1024).bytes;
        const auto attempt = [&](const NativePixelMergerState &state, const Json &quad, uint32_t lane = 0) {
            return mergeNativePixelOutput(replay.nativeDevice(), true, state, quad, lane);
        };
        QVERIFY_EXCEPTION_THROWN(
            mergeNativePixelOutput(replay.nativeDevice(), true, s, outputs, 0, [] { return true; }),
            OperationCancelled);
        auto changed = outputs;
        changed["adapter"]["luid"] = 0;
        QVERIFY_EXCEPTION_THROWN(attempt(s, changed), std::invalid_argument);
        changed = outputs;
        changed["lanes"][0]["outputs"][0]["valid_mask"] = 7;
        QVERIFY_EXCEPTION_THROWN(attempt(s, changed), std::runtime_error);
        changed = outputs;
        changed["lanes"][0]["shader_active"] = false;
        QVERIFY_EXCEPTION_THROWN(attempt(s, changed), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(attempt(s, outputs, 4), std::invalid_argument);
        auto invalid = s;
        invalid.colorFormat = 0;
        invalid.color.clear();
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        auto depthOnly = invalid;
        depthOnly.depthFormat = 40;
        depthOnly.depth = {0, 0, 0, 0x3f};
        depthOnly.rasterDepthBits = 0;
        changed = outputs;
        changed["lanes"][0]["outputs"] = Json::array();
        const auto withoutColor = attempt(depthOnly, changed);
        QCOMPARE(withoutColor.at("color_after"), Json::array());
        QCOMPARE(withoutColor.at("depth_after"), Json::array({0, 0, 0, 0}));
        invalid = depthOnly;
        invalid.color = {0};
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, changed), std::invalid_argument);
        invalid = depthOnly;
        invalid.depth.clear();
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, changed), std::invalid_argument);
        invalid = depthOnly;
        invalid.rasterDepthBits.reset();
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, changed), std::invalid_argument);
        invalid = depthOnly;
        invalid.blend.AlphaToCoverageEnable = TRUE;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, changed), std::runtime_error);
        invalid = depthOnly;
        invalid.blend.RenderTarget[0].BlendEnable = TRUE;
        invalid.blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC1_COLOR;
        QCOMPARE(attempt(invalid, changed).at("depth_after"), withoutColor.at("depth_after"));
        changed = outputs;
        changed["lanes"][0]["outputs"][0]["valid_mask"] = 0;
        QCOMPARE(attempt(depthOnly, changed).at("depth_after"), withoutColor.at("depth_after"));
        invalid = s;
        invalid.color.pop_back();
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        invalid = s;
        invalid.depthViewFlags = 1;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        invalid = s;
        invalid.depthFormat = 45;
        invalid.depth.resize(4);
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        invalid = s;
        invalid.timeoutMs = 60001;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        for (uint32_t extent : {0u, 16385u}) {
            invalid = s;
            invalid.width = extent;
            QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
            invalid = s;
            invalid.height = extent;
            QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        }
        invalid = s;
        invalid.x = 1;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        invalid = s;
        invalid.y = 1;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        invalid = s;
        invalid.maxBytes = 0;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        invalid.maxBytes = 95;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::runtime_error);
        for (UINT count : {0u, 3u, 64u}) {
            invalid = s;
            invalid.samples.Count = count;
            QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        }
        invalid = s;
        invalid.samples.Quality = 1;
        QVERIFY_EXCEPTION_THROWN(attempt(invalid, outputs), std::invalid_argument);
        unsigned checks = 0;
        QVERIFY_EXCEPTION_THROWN(
            mergeNativePixelOutput(replay.nativeDevice(), true, s, outputs, 0, [&] { return ++checks > 2; }),
            OperationCancelled);
        const auto a = attempt(s, outputs);
        const auto b = attempt(s, outputs);
        QCOMPARE(a.at("color_after"), b.at("color_after"));
        QCOMPARE(replay.observeTextureStorage(1000, 1024).bytes, s.color);
        auto multisample = s;
        multisample.samples.Count = 2;
        multisample.color.insert(multisample.color.end(), s.color.begin(), s.color.end());
        checks = 0;
        QVERIFY_EXCEPTION_THROWN(mergeNativePixelOutput(replay.nativeDevice(), true, multisample, outputs, 0,
                                                        [&] { return ++checks > 3; }),
                                 OperationCancelled);
        const auto resumed = attempt(multisample, outputs);
        QCOMPARE(resumed.at("color_after"), attempt(multisample, outputs).at("color_after"));
        QCOMPARE(replay.observeTextureStorage(1000, 1024).bytes, s.color);
    }
};
QTEST_GUILESS_MAIN(NativePixelMergerTests)
#include "NativePixelMergerTests.moc"
