#include "BlendCapture.h"
#include "DepthStencilCapture.h"
#include "application/HlslCompilation.h"
#include "application/NativePixelMerger.h"
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
void save(const Json &result, const Json &outputs) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_MERGER_EVIDENCE");
    if (root.isEmpty())
        return;
    QDir dir(root);
    if (!dir.exists() && !QDir().mkpath(root))
        throw std::runtime_error("Cannot create merger evidence directory");
    const auto text = Json{{"result", result}, {"quad_outputs", outputs}}.dump(2);
    QFile file(dir.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save merger evidence");
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
    options.blendEdits[event] = {state.blend, state.blendFactor, state.sampleMask};
    options.depthStencilEdits[event] = {state.depthStencil, state.stencilRef};
    options.before = true;
    Replay before(frame, options);
    const auto snapshot = snapshotResourceInputs(before, 4);
    const auto outputs = evaluateNativePixelOutputs(snapshot, {});
    state.color = before.observeTextureStorage(color, 1024).bytes;
    if (depth)
        state.depth = before.observeTextureStorage(depth, 1024).bytes;
    auto result = mergeNativePixelOutput(before.nativeDevice(), options.warp, state, outputs, 0);
    if (before.observeTextureStorage(color, 1024).bytes != state.color ||
        (depth && before.observeTextureStorage(depth, 1024).bytes != state.depth))
        throw std::runtime_error("Private merger changed reference storage");
    options.before = false;
    Replay after(frame, options);
    after.run();
    const auto expectedColor = after.observeTextureStorage(color, 1024).bytes;
    const auto expectedDepth =
        depth ? after.observeTextureStorage(depth, 1024).bytes : std::vector<uint8_t>();
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
        unsigned checks = 0;
        QVERIFY_EXCEPTION_THROWN(
            mergeNativePixelOutput(replay.nativeDevice(), true, s, outputs, 0, [&] { return ++checks > 2; }),
            OperationCancelled);
        const auto a = attempt(s, outputs);
        const auto b = attempt(s, outputs);
        QCOMPARE(a.at("color_after"), b.at("color_after"));
        QCOMPARE(replay.observeTextureStorage(1000, 1024).bytes, s.color);
    }
};
QTEST_GUILESS_MAIN(NativePixelMergerTests)
#include "NativePixelMergerTests.moc"
