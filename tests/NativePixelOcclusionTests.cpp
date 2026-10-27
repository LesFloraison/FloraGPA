#include "DepthStencilCapture.h"
#include "MsaaCapture.h"
#include "application/NativePixelOcclusion.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
ReplayOptions options(uint32_t vendor) {
    ReplayOptions o;
    o.debug = true;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    return o;
}
void devices() {
    QTest::addColumn<uint32_t>("vendor");
    QTest::newRow("warp") << 0u;
    QTest::newRow("nvidia") << 4318u;
    QTest::newRow("intel") << 32902u;
}
} // namespace
class NativePixelOcclusionTests final : public QObject {
    Q_OBJECT
  private slots:
    void restrictions_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint64_t>("expected");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto &[mode, count] : std::vector<std::pair<QString, uint64_t>>{{"ordinary", 1},
                                                                                       {"unchanged", 1},
                                                                                       {"scissor", 0},
                                                                                       {"depth", 0},
                                                                                       {"stencil", 0},
                                                                                       {"discard", 0},
                                                                                       {"early_discard", 1},
                                                                                       {"sample_mask", 0},
                                                                                       {"color_mask", 1},
                                                                                       {"disabled", 0},
                                                                                       {"suppressed", 0},
                                                                                       {"overlap", 2}})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << mode << count;
    }
    void restrictions() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        QFETCH(uint64_t, expected);
        auto capture = depthStencilCapture();
        auto o = options(vendor);
        const Id event = mode == "unchanged" ? 2000 : 1000;
        if (mode == "scissor") {
            D3D11_RASTERIZER_DESC2 d{};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = D3D11_CULL_NONE;
            d.DepthClipEnable = d.ScissorEnable = TRUE;
            o.rasterizerEdits[event].descriptor = d;
            o.rasterizerEdits[event].scissors = std::vector<D3D11_RECT>{{1, 1, 2, 2}};
        }
        if (mode == "depth" || mode == "stencil") {
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthEnable = TRUE;
            d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            d.DepthFunc = mode == "depth" ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_ALWAYS;
            d.StencilEnable = mode == "stencil";
            d.StencilReadMask = d.StencilWriteMask = 255;
            d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                        D3D11_COMPARISON_NEVER};
            o.depthStencilEdits[event].descriptor = d;
        }
        if (mode == "sample_mask")
            o.blendEdits[event].sampleMask = 0;
        if (mode == "color_mask") {
            D3D11_BLEND_DESC1 d{};
            for (auto &r : d.RenderTarget) {
                r.SrcBlend = r.SrcBlendAlpha = D3D11_BLEND_ONE;
                r.DestBlend = r.DestBlendAlpha = D3D11_BLEND_ZERO;
                r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                r.LogicOp = D3D11_LOGIC_OP_NOOP;
            }
            o.blendEdits[event].descriptor = d;
        }
        if (mode == "discard")
            o.shaders[32] = compileClassProgram("float4 main():SV_Target{discard;return 1;}", "ps_5_0");
        if (mode == "early_discard")
            o.shaders[32] = compileClassProgram(
                "[earlydepthstencil]float4 main(float4 p:SV_Position):SV_Target{if(p.x<2)discard;return 1;}",
                "ps_5_0");
        if (mode == "disabled")
            o.disabled.insert(event);
        if (mode == "suppressed")
            o.suppressDraws = true;
        if (mode == "overlap") {
            o.shaders[30] = compileClassProgram("float4 main(uint v:SV_VertexID):SV_Position{uint "
                                                "i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                                                "vs_5_0");
            for (const auto &entry : capture.entries)
                if (entry.id == event)
                    put(capture.bytes, size_t(entry.offset) + 24, 6u);
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                        D3D11_COMPARISON_ALWAYS};
            o.depthStencilEdits[event].descriptor = d;
        }
        // The repeated draw must pass depth to test writes with identical color.
        if (mode == "unchanged") {
            D3D11_DEPTH_STENCIL_DESC d{};
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
            d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                        D3D11_COMPARISON_ALWAYS};
            o.depthStencilEdits[event].descriptor = d;
        }
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        if (mode == "overlap")
            QCOMPARE(frame.event(event).args[0], 6u);
        const auto hash = frame.sha256();
        const auto result = measureNativePixelOcclusion(frame, o, {event});
        if (mode == "early_discard") {
            auto full = o;
            full.until = event;
            full.measurement = ReplayOptions::Measurement{event, event, true};
            Replay reference(frame, full);
            reference.run();
            QVERIFY(reference.measurementResult());
            expected = reference.measurementResult()->occlusion;
        }
        QCOMPARE(result.at("passing_samples").get<uint64_t>(), expected);
        QCOMPARE(result.at("debug_validation"), Json("no_errors"));
        QCOMPARE(result.at("frame_sha256"), Json(hash));
        QCOMPARE(result.at("additional_uav_slots"), Json(0));
        QVERIFY(result.at("original_fragment_eligibility").is_null());
        QVERIFY(result.at("fragment_records").is_null());
        // A fresh invocation must not inherit the previous diagnostic's writes.
        QCOMPARE(measureNativePixelOcclusion(frame, o, {event}).at("passing_samples"), Json(expected));
        QCOMPARE(frame.sha256(), hash);
    }
    void samples_data() { devices(); }
    void samples() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        msaaOutputCapture(false).save(path);
        Frame frame(path.toStdWString());
        for (Id event : {Id(100), Id(110)})
            for (uint32_t sample = 0; sample < 4; ++sample) {
                auto o = options(vendor);
                NativePixelOcclusionSelection s{event, 0, 3, 2, sample};
                QCOMPARE(measureNativePixelOcclusion(frame, o, s).at("passing_samples"), Json(1));
                o.blendEdits[event].sampleMask = ~(1u << sample);
                QCOMPARE(measureNativePixelOcclusion(frame, o, s).at("passing_samples"), Json(0));
            }
    }
    void earlyDiscardReference_data() { devices(); }
    void earlyDiscardReference() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        for (bool constant : {false, true}) {
            auto o = options(vendor);
            o.shaders[32] =
                compileClassProgram(constant ? "[earlydepthstencil]float4 main():SV_Target{discard;return 1;}"
                                             : "[earlydepthstencil]float4 main(float4 "
                                               "p:SV_Position):SV_Target{if(p.x<2)discard;return 1;}",
                                    "ps_5_0");
            const auto actual = measureNativePixelOcclusion(frame, o, {1000});
            // Independent full-draw query path: no region or sample-mask modification.
            o.until = 1000;
            o.measurement = ReplayOptions::Measurement{1000, 1000, true};
            Replay reference(frame, o);
            reference.run();
            QVERIFY(reference.measurementResult());
            const auto expected = reference.measurementResult()->occlusion;
            qInfo().noquote() << QString::fromStdString(Json{
                {"vendor", vendor},
                {"constant_discard", constant},
                {"original_draw_query", expected},
                {"selected_region_query",
                 actual.at("passing_samples")}}.dump());
            QCOMPARE(actual.at("passing_samples").get<uint64_t>(), expected);
        }
    }
    void forcedSamples_data() { devices(); }
    void forcedSamples() {
        QFETCH(uint32_t, vendor);
        auto capture = depthStencilCapture();
        capture.entries.erase(std::remove_if(capture.entries.begin(), capture.entries.end(),
                                             [](const auto &e) { return e.id == 990; }),
                              capture.entries.end());
        State state{};
        state.topology = 4;
        state.sampleMask = UINT32_MAX;
        state.omStart = 8;
        state.rtCount = 1;
        state.rtv[0] = 21;
        state.viewports = 22;
        state.stages[0].shader = 30;
        state.stages[4].shader = 32;
        capture.add(990, 3, 3, snapshot(state));
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        auto o = options(vendor);
        D3D11_RASTERIZER_DESC2 d{};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        d.ForcedSampleCount = 4;
        o.rasterizerEdits[1000].descriptor = d;
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth.FrontFace = depth.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                            D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        o.depthStencilEdits[1000].descriptor = depth;
        const auto result = measureNativePixelOcclusion(frame, o, {1000});
        auto full = o;
        full.until = 1000;
        full.measurement = ReplayOptions::Measurement{1000, 1000, true};
        Replay reference(frame, full);
        reference.run();
        QVERIFY(reference.measurementResult());
        const auto originalCount = reference.measurementResult()->occlusion;
        qInfo().noquote() << QString::fromStdString(Json{
            {"vendor", vendor},
            {"forced_samples", 4},
            {"original_draw_query", originalCount},
            {"selected_region_query", result.at("passing_samples")},
            {"raster_state", result.at("raster_state")}}.dump());
        QCOMPARE(result.at("passing_samples").get<uint64_t>(), originalCount);
        QCOMPARE(result.at("count_domain"), Json("forced_raster_samples"));
        QCOMPARE(result.at("sample_selection_domain"), Json("output_storage"));
        QVERIFY(result.at("forced_sample_mask_implementation_freedom").get<bool>());
        QVERIFY_EXCEPTION_THROWN(measureNativePixelOcclusion(frame, o, {1000, 0, 0, 0, 1}),
                                 std::runtime_error);
        o.blendEdits[1000].sampleMask = 0;
        const auto masked =
            measureNativePixelOcclusion(frame, o, {1000}).at("passing_samples").get<uint64_t>();
        // D3D11.3 section 20.4.6 explicitly permits either value for forced samples.
        QVERIFY(masked == 0 || masked == 4);
    }
    void predicate_data() { devices(); }
    void predicate() {
        QFETCH(uint32_t, vendor);
        for (bool visible : {false, true})
            for (uint32_t value : {0u, 1u}) {
                QTemporaryDir dir;
                const auto path = dir.filePath("frame.gpa_frame");
                predicateCapture(visible, value).save(path);
                Frame frame(path.toStdWString());
                QCOMPARE(measureNativePixelOcclusion(frame, options(vendor), {2000}).at("passing_samples"),
                         Json(visible != bool(value) ? 1 : 0));
            }
    }
    void failuresAndRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        depthStencilCapture().save(path);
        Frame frame(path.toStdWString());
        auto o = options(0);
        QVERIFY_EXCEPTION_THROWN(measureNativePixelOcclusion(frame, o, {1000}, [] { return true; }),
                                 OperationCancelled);
        int checks = 0;
        QVERIFY_EXCEPTION_THROWN(measureNativePixelOcclusion(frame, o, {1000}, [&] { return ++checks == 4; }),
                                 OperationCancelled);
        for (const auto &s : std::vector<NativePixelOcclusionSelection>{
                 {800}, {1000, 8}, {1000, 0, 1}, {1000, 0, 0, 0, 1}, {1000, 0, 0, 0, 0, 0}})
            QVERIFY_EXCEPTION_THROWN(measureNativePixelOcclusion(frame, o, s), std::exception);
        QCOMPARE(measureNativePixelOcclusion(frame, o, {1000}).at("passing_samples"), Json(1));
        o.renderdocLibrary = L"unavailable.dll";
        QVERIFY_EXCEPTION_THROWN(measureNativePixelOcclusion(frame, o, {1000}), std::invalid_argument);
    }
};
QTEST_GUILESS_MAIN(NativePixelOcclusionTests)
#include "NativePixelOcclusionTests.moc"
