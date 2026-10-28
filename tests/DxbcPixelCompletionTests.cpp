#include "BlendCapture.h"
#include "DepthStencilCapture.h"
#include "application/DxbcGpuCompletion.h"
#include "application/DxbcPixelCompletion.h"
#include "application/NativePixelGeometryReplay.h"
#include "replay/BlendState.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
const std::array<uint32_t, 3> marker{0x3f800000, 0x40000000, 0x40400000};
void floatTarget(Capture &c) {
    std::erase_if(c.entries, [](const auto &e) { return e.id == 20 || e.id == 21; });
    c.add(20, 5, 0x85, statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, 2u, 1u, 0u, 0u, 32u, 0u, 0u, Id(490)));
    c.add(490, 9, 1, statePack(16u, 0u, 0u, 0u, 0u));
    c.add(21, 5, 0x8d, statePack(Id(0), Id(0), Id(20), 2u, 4u, 0u, 0u, 0u));
}
void save(const Json &report, Bytes original = {}, Bytes marked = {}) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_COMPLETION_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create pixel completion evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                                   QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto text = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save pixel completion evidence");
    for (const auto &[suffix, bytes] :
         std::vector<std::pair<QString, Bytes>>{{".original.dxbc", original}, {".marked.dxbc", marked}}) {
        if (bytes.empty())
            continue;
        QFile binary(file.fileName() + suffix);
        if (!binary.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            binary.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                qint64(bytes.size()))
            throw std::runtime_error("Cannot save completion shader evidence");
    }
}
ReplayOptions options(uint32_t vendor, Id event) {
    ReplayOptions o;
    o.debug = true;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.until = event;
    o.measurement = ReplayOptions::Measurement{event, event, true};
    return o;
}
std::array<uint32_t, 4> privatePixel(Replay &r, Id resource, uint32_t sample, uint32_t samples,
                                     uint32_t format) {
    const auto bytes = samples == 1 ? r.readTexture(resource) : r.readMsaa(resource, sample, format).bytes;
    if (bytes.size() != 16)
        throw std::runtime_error("Unexpected private completion pixel size");
    std::array<uint32_t, 4> words{};
    std::memcpy(words.data(), bytes.data(), 16);
    return words;
}
void unchangedInstructions(Bytes original, Bytes marked, uint32_t target = 0) {
    auto code = [](Bytes bytes) {
        for (const auto &[tag, part] : readDxbcParts(bytes))
            if (tag == 0x52444853 || tag == 0x58454853)
                return readDxbcProgram(part);
        throw std::runtime_error("Missing completion test executable");
    };
    const auto a = code(original), b = code(marked);
    QCOMPARE(a.header[0], b.header[0]);
    std::vector<uint32_t> destination;
    for (const auto &row : a.instructions)
        if ((row[0] & 2047) == 101 && row.size() >= 3 && ((row[1] >> 12) & 255) == 2 && row.back() == target)
            destination.assign(row.begin() + 1, row.end());
    QVERIFY(!destination.empty());
    destination[0] = (destination[0] & ~0xf0u) | (7u << 4);
    std::vector<uint32_t> expectedMarker{uint32_t(6 + destination.size()) << 24 | 54u};
    expectedMarker.insert(expectedMarker.end(), destination.begin(), destination.end());
    expectedMarker.insert(expectedMarker.end(), {0x4002, marker[0], marker[1], marker[2], 0});
    size_t output = 0;
    bool mainBody = true;
    for (const auto &row : a.instructions) {
        const auto op = row[0] & 2047;
        if (op == 44)
            mainBody = false;
        if (mainBody && (op == 62 || op == 63)) {
            QVERIFY(output < b.instructions.size());
            const auto &added = b.instructions[output++];
            QCOMPARE(added, expectedMarker);
        }
        QVERIFY(output < b.instructions.size());
        auto observed = b.instructions[output++];
        if (op == 101 && row.size() >= 3 && ((row[1] >> 12) & 255) == 2 && row.back() == target)
            observed[1] = (observed[1] & ~0xf0u) | (row[1] & 0xf0u);
        QCOMPARE(observed, row);
    }
    QCOMPARE(output, b.instructions.size());
}
} // namespace
class DxbcPixelCompletionTests final : public QObject {
    Q_OBJECT
  private slots:
    void color_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 4u})
                for (const auto *mode : {"ordinary", "discard", "early-discard", "branch", "coverage",
                                         "alpha", "masked", "mrt", "partial", "integer", "signed", "minimum"})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(samples).arg(mode)))
                        << vendor << samples << QString(mode);
    }
    void color() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, samples);
            QFETCH(QString, mode);
            const uint32_t target = mode == "mrt" ? 7 : 0;
            const bool integer = mode == "integer" || mode == "signed", partial = mode == "partial";
            const uint32_t integerFormat = mode == "signed" ? 4 : 3;
            auto readPixel = [&](Replay &r, uint32_t sample) {
                return integer ? privatePixel(r, 1000 + target * 4, sample, samples, integerFormat)
                               : blendPixel(r, 1000 + target * 4, sample);
            };
            auto c = blendCapture(target + 1, false, integer, samples);
            if (mode == "signed")
                for (const auto &e : c.entries) {
                    if (e.id == 1000)
                        put(c.bytes, size_t(e.offset) + 32, 4u);
                    if (e.id == 1002)
                        put(c.bytes, size_t(e.offset) + 24, 4u);
                }
            std::string body = "return float4(.25,.5,.75,.375);";
            if (mode == "discard" || mode == "early-discard")
                body = "if(p.x<1)discard;" + body;
            if (mode == "branch")
                body = "if(p.x>0)return float4(.5,.75,.25,.375);" + body;
            std::string source = std::string(mode == "early-discard" ? "[earlydepthstencil]" : "") +
                                 "float4 main(float4 p:SV_Position):SV_Target" + std::to_string(target) +
                                 "{" + body + "}";
            if (mode == "coverage")
                source = "struct O{float4 c:SV_Target;uint m:SV_Coverage;};"
                         "O main(){O o;o.c=float4(.25,.5,.75,.375);o.m=5;return o;}";
            if (partial)
                source = "float main():SV_Target{return .25;}";
            if (integer)
                source = "uint4 main():SV_Target{return uint4(4,5,6,7);}";
            if (mode == "signed")
                source = "int4 main():SV_Target{return int4(-4,5,-6,7);}";
            if (mode == "minimum")
                source = "min16float4 main():SV_Target{return min16float4(.25,.5,.75,.375);}";
            const auto original = compileClassProgram(source, "ps_5_0");
            auto marked = instrumentPixelCompletion(original, target, marker);
            unchangedInstructions(original, marked.bytecode, target);
            QTemporaryDir dir;
            const auto path = dir.filePath("frame.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor, 100);
            o.shaders[12] = original;
            D3D11_BLEND_DESC1 blend{};
            blend.AlphaToCoverageEnable = mode == "alpha";
            blend.IndependentBlendEnable = TRUE;
            for (auto &rt : blend.RenderTarget) {
                rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                rt.RenderTargetWriteMask = 15;
            }
            o.blendEdits[100].descriptor = blend;
            if (mode == "masked")
                o.blendEdits[100].sampleMask = 0;
            auto initialOptions = o;
            initialOptions.before = true;
            initialOptions.measurement.reset();
            Replay before(f, initialOptions);
            before.run();
            std::vector<std::array<uint32_t, 4>> initial;
            for (uint32_t sample = 0; sample < samples; ++sample)
                initial.push_back(readPixel(before, sample));
            Replay reference(f, o);
            reference.run();
            std::vector<std::array<uint32_t, 4>> expected;
            for (uint32_t sample = 0; sample < samples; ++sample)
                expected.push_back(readPixel(reference, sample));
            if (integer)
                for (const auto &value : expected)
                    QCOMPARE(value,
                             (mode == "signed" ? std::array<uint32_t, 4>{uint32_t(-4), 5, uint32_t(-6), 7}
                                               : std::array<uint32_t, 4>{4, 5, 6, 7}));
            const auto count = reference.measurementResult()->occlusion;
            o.shaders[12] = marked.bytecode;
            Replay observed(f, o);
            observed.run();
            QCOMPARE(observed.measurementResult()->occlusion, count);
            Json values = Json::array();
            uint32_t survivors = 0;
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const auto actual = readPixel(observed, sample);
                const bool wrote = expected[sample][0] != initial[sample][0];
                if (wrote) {
                    for (unsigned component = 0; component < 3; ++component)
                        QCOMPARE(actual[component], marker[component]);
                    if (!partial)
                        QCOMPARE(actual[3], expected[sample][3]);
                    ++survivors;
                } else
                    QCOMPARE(actual, initial[sample]);
                values.push_back({{"sample", sample},
                                  {"before", initial[sample]},
                                  {"reference", expected[sample]},
                                  {"marked", actual},
                                  {"marker_survived", wrote}});
            }
            if (mode != "early-discard")
                QCOMPARE(uint64_t(survivors), count);
            Json privateValues = Json::array();
            auto observation = replayNativePixelCompletion(before, 100, target, [&](Replay &p, bool after) {
                if (after)
                    for (uint32_t sample = 0; sample < samples; ++sample)
                        privateValues.push_back(
                            privatePixel(p, 1000 + target * 4, sample, samples, integer ? integerFormat : 2));
            });
            QCOMPARE(observation.at("passing_samples"), Json(count));
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const auto actual = privateValues[sample].get<std::array<uint32_t, 4>>();
                if (values[sample].at("marker_survived") == true) {
                    for (unsigned component = 0; component < 3; ++component)
                        QCOMPARE(actual[component], marker[component]);
                    if (!partial)
                        QCOMPARE(actual[3], expected[sample][3]);
                } else
                    QCOMPARE(actual, (std::array<uint32_t, 4>{}));
                QCOMPARE(
                    privatePixel(before, 1000 + target * 4, sample, samples, integer ? integerFormat : 2),
                    initial[sample]);
            }
            observation["selected_samples"] = privateValues;
            marked.identity.update({{"status", "ok"},
                                    {"vendor_request", vendor},
                                    {"samples", samples},
                                    {"mode", mode.toStdString()},
                                    {"values", values},
                                    {"reference_passing_samples", count},
                                    {"marker_samples", survivors},
                                    {"private_observation", observation},
                                    {"original_storage_restored", true}});
            save(marked.identity, original, marked.bytecode);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void depth_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"depth-pass", "depth-fail", "stencil-pass", "stencil-fail", "discard",
                                     "early-discard", "shader-depth", "early-stencil-discard"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void depth() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            auto c = depthStencilCapture();
            floatTarget(c);
            const bool early = mode.startsWith("early"), discard = mode.contains("discard");
            std::string source = std::string(early ? "[earlydepthstencil]" : "") +
                                 "float4 main():SV_Target{" + (discard ? std::string("discard;") : "") +
                                 "return float4(.25,.5,.75,.375);}";
            if (mode == "shader-depth")
                source = "struct O{float4 c:SV_Target;float d:SV_Depth;};"
                         "O main(){O o;o.c=float4(.25,.5,.75,.375);o.d=.75;return o;}";
            const auto original = compileClassProgram(source, "ps_5_0");
            auto marked = instrumentPixelCompletion(original, 0, marker);
            unchangedInstructions(original, marked.bytecode);
            QTemporaryDir dir;
            const auto path = dir.filePath("depth.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor, 1000);
            o.shaders[32] = original;
            D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = TRUE;
            ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            ds.DepthFunc = mode == "depth-fail" ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_LESS;
            ds.StencilEnable = mode.contains("stencil");
            ds.StencilReadMask = ds.StencilWriteMask = 255;
            ds.FrontFace =
                ds.BackFace = {D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
                               mode == "stencil-fail" ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_ALWAYS};
            o.depthStencilEdits[1000].descriptor = ds;
            Replay reference(f, o);
            reference.run();
            const auto depth = reference.readTexture(720), color = reference.readTexture(20);
            const auto count = reference.measurementResult()->occlusion;
            auto directOptions = o;
            directOptions.before = directOptions.prepareBeforeDraw = true;
            directOptions.measurement.reset();
            Replay direct(f, directOptions);
            direct.run();
            const auto initialDepth = direct.readTexture(720);
            uint64_t directCount = 0;
            direct.inspectNativeState([&](auto *context, const auto &) {
                Com<ID3D11Device> device;
                context->GetDevice(&device);
                Com<ID3D11Query> query;
                const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
                check(device->CreateQuery(&desc, &query), "Create independent completion query");
                context->Begin(query.Get());
                context->Draw(3, 0);
                context->End(query.Get());
                dxbc_exec::GpuCompletion completion(device.Get(), context);
                completion.submit();
                completion.query(query.Get(), &directCount, sizeof directCount);
            });
            QCOMPARE(directCount, count);
            QCOMPARE(direct.readTexture(20), color);
            QCOMPARE(direct.readTexture(720), depth);
            auto expectedDepth = initialDepth;
            if (mode != "depth-fail" && mode != "stencil-fail" && mode != "discard" && mode != "shader-depth")
                std::fill(expectedDepth.begin(), expectedDepth.begin() + 3, uint8_t(0));
            if (mode == "stencil-pass" || mode == "early-stencil-discard")
                expectedDepth[3] = uint8_t(~initialDepth[3]);
            if (mode == "stencil-fail")
                expectedDepth[3] = uint8_t(initialDepth[3] + 1);
            QCOMPARE(depth, expectedDepth);
            o.shaders[32] = marked.bytecode;
            Replay observed(f, o);
            observed.run();
            const auto actualDepth = observed.readTexture(720), actual = observed.readTexture(20);
            QCOMPARE(actualDepth, depth);
            QCOMPARE(observed.measurementResult()->occlusion, count);
            const bool survived =
                !discard && mode != "depth-fail" && mode != "stencil-fail" && mode != "shader-depth";
            if (survived) {
                QCOMPARE(actual.size(), size_t(16));
                QVERIFY(std::memcmp(actual.data(), marker.data(), 12) == 0);
                QVERIFY(std::memcmp(actual.data() + 12, color.data() + 12, 4) == 0);
            } else
                QCOMPARE(actual, color);
            Replay privateReplay(f, directOptions);
            privateReplay.run();
            const auto privateColor = privateReplay.readTexture(20);
            std::vector<uint8_t> privateAfter, privateDepth;
            auto observation =
                replayNativePixelCompletion(privateReplay, 1000, 0, [&](Replay &p, bool after) {
                    if (after) {
                        privateAfter = p.readTexture(20);
                        privateDepth = p.readTexture(720);
                    }
                });
            QCOMPARE(privateDepth, depth);
            QCOMPARE(observation.at("passing_samples"), Json(count));
            QCOMPARE(privateReplay.readTexture(720), initialDepth);
            QCOMPARE(privateReplay.readTexture(20), privateColor);
            if (survived)
                QCOMPARE(privateAfter, actual);
            else
                QCOMPARE(privateAfter, std::vector<uint8_t>(16));
            observation["selected_color"] = privateAfter;
            observation["selected_depth"] = privateDepth;
            marked.identity.update(
                {{"status", "ok"},
                 {"vendor_request", vendor},
                 {"mode", mode.toStdString()},
                 {"reference_depth", depth},
                 {"marked_depth", actualDepth},
                 {"reference_color", color},
                 {"marked_color", actual},
                 {"reference_passing_samples", count},
                 {"marker_survived", survived},
                 {"direct_native_passing_samples", directCount},
                 {"initial_depth", initialDepth},
                 {"expected_depth", expectedDepth},
                 {"private_observation", observation},
                 {"original_storage_restored", true},
                 {"early_occlusion_contract_match", early ? Json(count == 1) : Json(nullptr)}});
            save(marked.identity, original, marked.bytecode);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void sideEffects_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"append", "counter", "classes"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void sideEffects() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            auto c = mode == "classes" ? graphicsClassCapture() : graphicsCounterCapture(mode == "counter");
            floatTarget(c);
            QTemporaryDir dir;
            const auto path = dir.filePath("effects.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            const auto original = f.shader(f.resource(32).data);
            auto marked = instrumentPixelCompletion(original, 0, marker);
            unchangedInstructions(original, marked.bytecode);
            auto o = options(vendor, 200);
            Replay reference(f, o);
            reference.run();
            const auto buffer = reference.readBuffer(10), color = reference.readTexture(20);
            const auto counter = reference.readCounter(12);
            o.shaders[32] = marked.bytecode;
            Replay observed(f, o);
            observed.run();
            const auto actual = observed.readTexture(20);
            QCOMPARE(observed.readBuffer(10), buffer);
            QCOMPARE(observed.readCounter(12), counter);
            QCOMPARE(observed.measurementResult()->occlusion, reference.measurementResult()->occlusion);
            QVERIFY(std::memcmp(actual.data(), marker.data(), 12) == 0);
            QVERIFY(std::memcmp(actual.data() + 12, color.data() + 12, 4) == 0);
            o.shaders.erase(32);
            o.before = o.prepareBeforeDraw = true;
            o.measurement.reset();
            Replay isolated(f, o);
            isolated.run();
            const auto initialBuffer = isolated.readBuffer(10), initialColor = isolated.readTexture(20);
            const auto initialCounter = isolated.readCounter(12);
            std::vector<uint8_t> privateBuffer, privateColor;
            uint32_t privateCounter = 0;
            auto observation = replayNativePixelCompletion(isolated, 200, 0, [&](Replay &p, bool after) {
                if (after) {
                    privateBuffer = p.readBuffer(10);
                    privateColor = p.readTexture(20);
                    privateCounter = p.readCounter(12);
                }
            });
            QCOMPARE(privateBuffer, buffer);
            QCOMPARE(privateColor, actual);
            QCOMPARE(privateCounter, counter);
            QCOMPARE(isolated.readBuffer(10), initialBuffer);
            QCOMPARE(isolated.readTexture(20), initialColor);
            QCOMPARE(isolated.readCounter(12), initialCounter);
            observation["selected_color"] = privateColor;
            observation["uav_storage"] = privateBuffer;
            observation["hidden_counter"] = privateCounter;
            marked.identity.update({{"status", "ok"},
                                    {"vendor_request", vendor},
                                    {"mode", mode.toStdString()},
                                    {"reference_buffer", buffer},
                                    {"marked_buffer", observed.readBuffer(10)},
                                    {"counter", counter},
                                    {"reference_color", color},
                                    {"marked_color", actual},
                                    {"private_observation", observation},
                                    {"original_storage_restored", true}});
            save(marked.identity, original, marked.bytecode);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void merger_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode :
                 {"shared-blend", "independent-blend", "logic", "write-mask", "same-value", "dual-source"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void merger() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            const bool multi = mode.endsWith("blend"), logical = mode == "logic";
            auto capture = blendCapture(multi ? 2 : 1, mode == "dual-source", logical);
            QTemporaryDir dir;
            const auto path = dir.filePath("merger.gpa_frame");
            capture.save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor, 100);
            const auto bytes = f.shader(f.resource(12).data);
            std::vector<uint8_t> original(bytes.begin(), bytes.end());
            if (mode == "same-value") {
                original =
                    compileClassProgram("float4 main():SV_Target{return float4(.125,.5,.25,.25);}", "ps_5_0");
                o.shaders[12] = original;
            }
            auto blend = defaultBlend();
            auto &selected = blend.RenderTarget[0];
            if (multi || mode == "dual-source") {
                selected.BlendEnable = TRUE;
                selected.SrcBlend = mode == "dual-source" ? D3D11_BLEND_SRC1_COLOR : D3D11_BLEND_SRC_ALPHA;
                selected.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                blend.IndependentBlendEnable = mode == "independent-blend";
            } else if (logical) {
                selected.LogicOpEnable = TRUE;
                selected.LogicOp = D3D11_LOGIC_OP_NOOP;
            } else if (mode == "write-mask")
                selected.RenderTargetWriteMask = 0;
            o.blendEdits[100].descriptor = blend;
            Replay reference(f, o);
            reference.run();
            const auto expected = privatePixel(reference, 1000, 0, 1, logical ? 3 : 2);
            const auto expectedOther =
                multi ? privatePixel(reference, 1004, 0, 1, 2) : std::array<uint32_t, 4>{};
            const auto referenceCount = reference.measurementResult()->occlusion;
            auto diagnosticOptions = o;
            auto shader = instrumentPixelCompletion(original, 0, marker);
            diagnosticOptions.shaders[12] = shader.bytecode;
            auto diagnosticBlend = blend;
            diagnosticBlend.IndependentBlendEnable = TRUE;
            if (multi && !blend.IndependentBlendEnable)
                diagnosticBlend.RenderTarget[1] = selected;
            diagnosticBlend.RenderTarget[0] = defaultBlend().RenderTarget[0];
            diagnosticOptions.blendEdits[100].descriptor = diagnosticBlend;
            Replay diagnostic(f, diagnosticOptions);
            diagnostic.run();
            const auto diagnosticCount = diagnostic.measurementResult()->occlusion;
            const auto diagnosticPixel = privatePixel(diagnostic, 1000, 0, 1, logical ? 3 : 2);
            if (multi) {
                const auto values = mode == "shared-blend" ? std::array<float, 4>{.3125f, .375f, .5f, .5f}
                                                           : std::array<float, 4>{.5f, .25f, .75f, .5f};
                for (size_t i = 0; i < 4; ++i)
                    QCOMPARE(expectedOther[i], std::bit_cast<uint32_t>(values[i]));
            }
            o.before = o.prepareBeforeDraw = true;
            o.measurement.reset();
            Replay direct(f, o);
            direct.run();
            uint64_t directCount = 0;
            direct.inspectNativeState([&](auto *context, const auto &) {
                Com<ID3D11Device> device;
                context->GetDevice(&device);
                Com<ID3D11Query> query;
                const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
                check(device->CreateQuery(&desc, &query), "Create independent merger query");
                context->Begin(query.Get());
                context->Draw(3, 0);
                context->End(query.Get());
                dxbc_exec::GpuCompletion completion(device.Get(), context);
                completion.submit();
                completion.query(query.Get(), &directCount, sizeof directCount);
            });
            QCOMPARE(directCount, referenceCount);
            QCOMPARE(privatePixel(direct, 1000, 0, 1, logical ? 3 : 2), expected);
            if (multi)
                QCOMPARE(privatePixel(direct, 1004, 0, 1, 2), expectedOther);
            Replay r(f, o);
            r.run();
            const auto before = privatePixel(r, 1000, 0, 1, logical ? 3 : 2);
            const auto otherBefore = multi ? privatePixel(r, 1004, 0, 1, 2) : std::array<uint32_t, 4>{};
            std::array<uint32_t, 4> marked{}, other{}, cleared{};
            auto observation = replayNativePixelCompletion(r, 100, 0, [&](Replay &p, bool after) {
                (after ? marked : cleared) = privatePixel(p, 1000, 0, 1, logical ? 3 : 2);
                if (after && multi)
                    other = privatePixel(p, 1004, 0, 1, 2);
            });
            QCOMPARE(cleared, (std::array<uint32_t, 4>{}));
            for (size_t i = 0; i < 3; ++i)
                QCOMPARE(marked[i], marker[i]);
            QCOMPARE(marked[3],
                     logical ? 0x80000001u : std::bit_cast<uint32_t>(mode == "same-value" ? .25f : .5f));
            QCOMPARE(other, expectedOther);
            QCOMPARE(marked, diagnosticPixel);
            QCOMPARE(privatePixel(r, 1000, 0, 1, logical ? 3 : 2), before);
            if (multi)
                QCOMPARE(privatePixel(r, 1004, 0, 1, 2), otherBefore);
            const bool unchanged = logical || mode == "write-mask" || mode == "same-value";
            QCOMPARE(expected == before, unchanged);
            QCOMPARE(observation.at("passing_samples"), Json(diagnosticCount));
            shader.identity.update(
                {{"status", "ok"},
                 {"vendor_request", vendor},
                 {"mode", mode.toStdString()},
                 {"merger",
                  {{"before", before},
                   {"reference", expected},
                   {"marked", marked},
                   {"cleared", cleared},
                   {"other", other},
                   {"reference_other", expectedOther},
                   {"original_value_unchanged", unchanged}}},
                 {"reference_passing_samples", referenceCount},
                 {"diagnostic_passing_samples", diagnosticCount},
                 {"direct_native_passing_samples", directCount},
                 {"single_sample_query_contract_match", referenceCount == 1 && diagnosticCount == 1},
                 {"diagnostic_query_matches_original", diagnosticCount == referenceCount},
                 {"private_observation", observation},
                 {"original_storage_restored", true}});
            save(shader.identity, original, shader.bytecode);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void recovery_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void recovery() {
        QFETCH(uint32_t, vendor);
        auto capture = graphicsCounterCapture(true);
        floatTarget(capture);
        QTemporaryDir dir;
        const auto path = dir.filePath("recovery.gpa_frame");
        capture.save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 200);
        Replay reference(f, o);
        reference.run();
        const auto expectedColor = reference.readTexture(20), expectedBuffer = reference.readBuffer(10);
        const auto expectedCounter = reference.readCounter(12);
        o.before = o.prepareBeforeDraw = true;
        o.measurement.reset();
        Replay r(f, o);
        r.run();
        const auto color = r.readTexture(20), buffer = r.readBuffer(10);
        const auto counter = r.readCounter(12);
        auto restored = [&] {
            return r.readTexture(20) == color && r.readBuffer(10) == buffer && r.readCounter(12) == counter;
        };
        for (bool boundary : {false, true}) {
            bool cancel = false;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     replayNativePixelCompletion(
                                         r, 200, 0, [&](Replay &, bool after) { cancel = after == boundary; },
                                         256ull * 1024 * 1024, 30000, [&] { return cancel; }));
            QVERIFY(restored());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     replayNativePixelCompletion(r, 200, 0, [&](Replay &, bool after) {
                                         if (after == boundary)
                                             throw std::runtime_error("Completion observer failure");
                                     }));
            QVERIFY(restored());
        }
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, replayNativePixelCompletion(r, 200, 8));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayNativePixelCompletion(r, 200, 1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayNativePixelCompletion(r, 200, 0, {}, 1));
        try {
            replayNativePixelCompletion(
                r, 200, 0, [](Replay &, bool) { QTest::qSleep(30); }, 256ull * 1024 * 1024, 20);
            QFAIL("Expired pixel completion deadline was accepted");
        } catch (const OperationCancelled &) {
            QFAIL("Completion deadline was reported as cancellation");
        } catch (const std::runtime_error &e) {
            QCOMPARE(std::string(e.what()), std::string("Native pixel geometry replay deadline"));
        }
        QVERIFY(restored());
        auto report = replayNativePixelCompletion(r, 200, 0);
        QCOMPARE(report.at("passing_samples"), Json(1));
        QVERIFY(restored());
        r.inspectNativeState([](auto *context, const auto &) { context->Draw(3, 0); });
        QCOMPARE(r.readTexture(20), expectedColor);
        QCOMPARE(r.readBuffer(10), expectedBuffer);
        QCOMPARE(r.readCounter(12), expectedCounter);
        report["status"] = "ok";
        report["recovered_failures"] = 8;
        report["original_storage_restored"] = true;
        report["original_binding_draw_matches"] = true;
        save(report);
    }
    void boundaries() {
        const auto shader = compileClassProgram("float4 main():SV_Target{return 1;}", "ps_5_0");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, instrumentPixelCompletion(shader, 8, marker));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentPixelCompletion(shader, 1, marker));
        const auto vertex = compileClassProgram("float4 main():SV_Position{return 1;}", "vs_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentPixelCompletion(vertex, 0, marker));
        const auto depth = compileClassProgram("float main():SV_Depth{return .5;}", "ps_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentPixelCompletion(depth, 0, marker));
        QVERIFY_THROWS_EXCEPTION(std::exception,
                                 instrumentPixelCompletion(Bytes(shader).first(12), 0, marker));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentPixelCompletion(shader, 0, marker, 1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 instrumentPixelCompletion(shader, 0, marker, shader.size()));
        QVERIFY_THROWS_EXCEPTION(
            OperationCancelled,
            instrumentPixelCompletion(shader, 0, marker, 256ull * 1024 * 1024, [] { return true; }));
        unsigned checks = 0;
        QVERIFY_THROWS_EXCEPTION(
            OperationCancelled,
            instrumentPixelCompletion(shader, 0, marker, 256ull * 1024 * 1024, [&] { return ++checks > 5; }));
        QVERIFY(checks > 5);
        for (const auto *profile : {"ps_4_0", "ps_4_1", "ps_5_0"}) {
            const auto original = compileClassProgram("float4 main():SV_Target{return 1;}", profile);
            const auto result = instrumentPixelCompletion(original, 0, marker);
            unchangedInstructions(original, result.bytecode);
        }
    }
};
QTEST_GUILESS_MAIN(DxbcPixelCompletionTests)
#include "DxbcPixelCompletionTests.moc"
