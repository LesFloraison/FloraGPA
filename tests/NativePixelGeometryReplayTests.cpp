#include "BlendCapture.h"
#include "DepthStencilCapture.h"
#include "StreamCapture.h"
#include "application/NativePixelDrawHistory.h"
#include "application/NativePixelGeometryReplay.h"
#include "replay/BlendState.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
void verifyMergerJoin(const Json &report) {
    QCOMPARE(report.at("merger_evaluation").at("requested"), Json(true));
    QCOMPARE(report.at("merger_evaluation").at("invocations"),
             report.at("shader_evaluation").at("invocations"));
    for (const auto &row : report.at("records")) {
        QCOMPARE(row.at("private_output_mergers").size(), row.at("private_shader_outputs").size());
        for (const auto &merger : row.at("private_output_mergers")) {
            QCOMPARE(merger.at("observed_value_matches_checkpoint"), Json(true));
            QCOMPARE(merger.at("exact_storage_matches_checkpoint"), Json(true));
            QVERIFY(merger.at("original_fragment_eligibility").is_null());
        }
    }
}
ReplayOptions options(uint32_t vendor, Id event) {
    ReplayOptions o;
    o.debug = true;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.until = event;
    return o;
}
void devices() {
    QTest::addColumn<uint32_t>("vendor");
    for (uint32_t vendor : {0u, 4318u, 32902u})
        QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
}
ID3D11DeviceContext *context(Replay &r) {
    ID3D11DeviceContext *result = nullptr;
    r.inspectNativeState([&](auto *native, const auto &) { result = native; });
    return result;
}
void drawCount(Capture &c, Id event, uint32_t count) {
    for (const auto &e : c.entries)
        if (e.id == event)
            put(c.bytes, size_t(e.offset) + 24, count);
}
void evidence(const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create geometry replay evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                                   QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto data = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(data.data(), qint64(data.size())) != qint64(data.size()))
        throw std::runtime_error("Cannot save geometry replay evidence");
}
D3D11_DEPTH_STENCIL_DESC depthState() {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.StencilReadMask = d.StencilWriteMask = 255;
    d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                D3D11_COMPARISON_ALWAYS};
    return d;
}
Capture layeredHistoryCapture(bool volume) {
    auto c = blendCapture();
    c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                   [](const auto &e) {
                                       return e.id == 1000 || e.id == 1001 || e.id == 1002 || e.id == 50 ||
                                              e.category == 3 || e.category == 7;
                                   }),
                    c.entries.end());
    const auto size = volume ? 1152u : 960u;
    c.add(1000, 5, volume ? 0x86 : 0x85,
          volume ? statePack(Id(0), Id(0), 4u, 4u, 4u, 2u, 2u, 0u, 32u, 0u, 0u, Id(1001))
                 : statePack(Id(0), Id(0), 4u, 4u, 2u, 3u, 2u, 1u, 0u, 0u, 32u, 0u, 0u, Id(1001)));
    auto data = word(size);
    data.resize(size + 4);
    c.add(1001, 9, 1, data);
    c.add(1002, 5, 0x8d, statePack(Id(0), Id(0), Id(1000), 2u, volume ? 8u : 5u, 1u, volume ? 0u : 1u, 2u));
    c.add(50, 9, 0x87, statePack(1u, 0.f, 0.f, 2.f, 2.f, 0.f, 1.f));
    const auto code = compileClassProgram(
        "struct V{float4 p:SV_Position;};struct O{float4 p:SV_Position;uint "
        "layer:SV_RenderTargetArrayIndex;};"
        "[maxvertexcount(3)]void main(triangle V v[3],uint id:SV_PrimitiveID,inout TriangleStream<O> output){"
        "[unroll]for(uint i=0;i<3;i++){O o;o.p=v[i].p;o.layer=id;output.Append(o);}}",
        "gs_5_0");
    c.add(60, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(61)));
    auto raw = statePack(uint64_t(code.size()));
    raw.insert(raw.end(), code.begin(), code.end());
    append(raw, Id(0));
    c.add(61, 9, 0x81, raw);
    State s{};
    s.topology = 4;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = 1;
    s.rtv[0] = 1002;
    s.rasterizer = 52;
    s.viewports = 50;
    s.stages[0].shader = 10;
    s.stages[3].shader = 60;
    s.stages[4].shader = 12;
    c.add(90, 3, 3, snapshot(s));
    c.add(100, 7, 0x37, statePack(Id(90), Id(0), Id(1), 6u, 0u));
    return c;
}
} // namespace
class NativePixelGeometryReplayTests final : public QObject {
    Q_OBJECT
  private slots:
    void mergerHistory_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 4u})
                for (const auto *mode : {"blend", "dual", "alpha", "write-mask", "unchanged"})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(samples).arg(mode)))
                        << vendor << samples << QString(mode);
    }
    void mergerHistory() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, samples);
        QFETCH(QString, mode);
        auto capture = blendCapture(1, mode == "dual", false, samples);
        drawCount(capture, 100, 6);
        auto o = options(vendor, 100);
        o.shaders[10] = compileClassProgram(
            "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
            "vs_5_0");
        if (mode == "unchanged")
            o.shaders[12] =
                compileClassProgram("float4 main():SV_Target{return float4(.125,.5,.25,.25);}", "ps_5_0");
        auto blend = defaultBlend();
        auto &rt = blend.RenderTarget[0];
        if (mode == "blend" || mode == "dual") {
            rt.BlendEnable = TRUE;
            rt.SrcBlend = mode == "dual" ? D3D11_BLEND_SRC1_COLOR : D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = mode == "dual" ? D3D11_BLEND_INV_SRC1_COLOR : D3D11_BLEND_INV_SRC_ALPHA;
        }
        blend.AlphaToCoverageEnable = mode == "alpha";
        if (mode == "write-mask")
            rt.RenderTargetWriteMask = 5;
        o.blendEdits[100].descriptor = blend;
        QTemporaryDir directory;
        const auto path = directory.filePath("frame.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        const uint32_t sample = samples - 1;
        Json prefixes = Json::array();
        for (uint32_t count : {1u, 2u}) {
            auto prefix = capture;
            drawCount(prefix, 100, count * 3);
            const auto prefixPath = directory.filePath(QString("prefix-%1.gpa_frame").arg(count));
            prefix.save(prefixPath);
            Frame prefixFrame(prefixPath.toStdWString());
            Replay reference(prefixFrame, o);
            reference.run();
            prefixes.push_back(blendPixel(reference, 1000, sample));
        }
        o.before = o.prepareBeforeDraw = true;
        Replay replay(frame, o);
        replay.run();
        NativeHistorySelection selection;
        selection.resource = 1000;
        selection.sample = sample;
        NativePixelDrawHistoryLimits limits;
        limits.evaluateShaderOutputs = limits.evaluateOutputMerger = true;
        auto report = collectNativePixelDrawHistory(replay, 100, selection, limits);
        verifyMergerJoin(report);
        QCOMPARE(report.at("records").size(), size_t(2));
        for (size_t i = 0; i < 2; ++i) {
            const auto &row = report.at("records")[i];
            const auto bytes = row.at("after").at("bytes").get<std::vector<uint8_t>>();
            QCOMPARE(bytes.size(), size_t(16));
            std::array<uint32_t, 4> words{};
            std::memcpy(words.data(), bytes.data(), 16);
            QCOMPARE(Json(words), prefixes[i]);
            if (mode == "unchanged") {
                QCOMPARE(row.at("observed_value_changed"), Json(false));
                QCOMPARE(row.at("private_output_mergers")[0].at("merger").at("passing_samples"), Json(1));
            }
        }
        report["status"] = "ok";
        report["mode"] = mode.toStdString();
        report["reference_prefix_bits"] = prefixes;
        evidence(report);
    }
    void shaderHistoryBoundaries_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"depth_reject", "stencil_reject", "null_ps", "forced"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void shaderHistoryBoundaries() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        const bool forced = mode == "forced";
        const Id event = forced ? 100 : 1000, resource = forced ? 1000 : mode == "null_ps" ? 720 : 20;
        auto c = forced ? blendCapture() : depthStencilCapture();
        auto o = options(vendor, event);
        if (mode == "null_ps") {
            std::erase_if(c.entries, [](const auto &e) { return e.id == 990; });
            State s{};
            s.topology = 4;
            s.sampleMask = UINT32_MAX;
            s.omStart = 8;
            s.dsv = 722;
            s.viewports = 22;
            s.rasterizer = 724;
            s.stages[0].shader = 30;
            c.add(990, 3, 3, snapshot(s));
        }
        auto depth = depthState();
        if (mode == "depth_reject") {
            depth.DepthEnable = TRUE;
            depth.DepthFunc = D3D11_COMPARISON_NEVER;
        }
        if (mode == "stencil_reject") {
            depth.StencilEnable = TRUE;
            depth.FrontFace.StencilFunc = depth.BackFace.StencilFunc = D3D11_COMPARISON_NEVER;
        }
        o.depthStencilEdits[event].descriptor = depth;
        if (forced) {
            D3D11_RASTERIZER_DESC2 d{};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = D3D11_CULL_NONE;
            d.DepthClipEnable = TRUE;
            d.ForcedSampleCount = 4;
            o.rasterizerEdits[event].descriptor = d;
        }
        o.shaders[forced ? 12 : 32] =
            compileClassProgram("float4 main():SV_Target{return float4(.25,.125,.375,.5);}", "ps_5_0");
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readTexture(resource);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readTexture(resource);
        NativeHistorySelection selection;
        selection.resource = resource;
        NativePixelDrawHistoryLimits limits;
        limits.evaluateShaderOutputs = true;
        limits.evaluateOutputMerger = true;
        auto report = collectNativePixelDrawHistory(r, event, selection, limits);
        verifyMergerJoin(report);
        QCOMPARE(report.at("records").size(), size_t(1));
        const auto &row = report.at("records")[0];
        QCOMPARE(row.at("after").at("bytes").get<std::vector<uint8_t>>(), expected);
        QCOMPARE(row.at("pixel_shader_bound"), Json(mode != "null_ps"));
        QCOMPARE(row.at("private_shader_outputs").size(), mode == "null_ps" ? size_t(0) : size_t(1));
        if (mode != "null_ps") {
            const auto &out = row.at("private_shader_outputs")[0];
            const auto &value = out.at("lanes")[out.at("selected_lane").get<size_t>()].at("outputs")[0];
            QCOMPARE(value.at("raw_bits"), Json::array({0x3e800000u, 0x3e000000u, 0x3ec00000u, 0x3f000000u}));
            QCOMPARE(value.at("valid_mask"), Json(15));
            QVERIFY(out.at("original_fragment_eligibility").is_null());
            if (forced) {
                QCOMPARE(out.at("candidate_raster_samples"), Json::array({0, 1, 2, 3}));
                QCOMPARE(out.at("input_provenance").at("equivalent_sample_observations").size(), size_t(3));
            } else
                QCOMPARE(row.at("observed_value_changed"), Json(false));
        }
        QCOMPARE(r.readTexture(resource), initial);
        report["status"] = "ok";
        report["mode"] = mode.toStdString();
        evidence(report);
    }

    void shaderHistory_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint32_t>("sample");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode :
                 {"constant", "coverage", "varying", "derivative", "sample", "masked", "discard"})
                for (uint32_t sample : {0u, 3u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(mode).arg(sample)))
                        << vendor << QString(mode) << sample;
    }
    void shaderHistory() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        QFETCH(uint32_t, sample);
        const uint32_t samples = sample ? 4 : 1;
        auto c = blendCapture(1, false, false, samples);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        const auto signature = mode == "sample"     ? "uint s:SV_SampleIndex"
                               : mode == "coverage" ? "uint c:SV_Coverage"
                               : mode == "constant" ? ""
                                                    : "float4 p:SV_Position";
        const auto body = mode == "sample"       ? "return float4(s*.25,.125,.375,.5);"
                          : mode == "coverage"   ? "return float4(c,.125,.375,.5);"
                          : mode == "constant"   ? "return float4(.25,.125,.375,.5);"
                          : mode == "derivative" ? "return float4(ddx_fine(p.x),ddy_fine(p.y),p.xy);"
                          : mode == "discard"    ? "if(p.x<1)discard;return float4(p.xy,.375,.5);"
                                                 : "return float4(p.xy,.375,.5);";
        o.shaders[12] = compileClassProgram(
            std::string("float4 main(") + signature + "):SV_Target{" + body + "}", "ps_5_0");
        if (mode == "masked")
            o.blendEdits[100].sampleMask = 0;
        Replay original(f, o);
        original.run();
        const auto gpu = blendPixel(original, 1000, sample);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = samples == 1 ? r.readTexture(1000) : r.readMsaa(1000, sample, 2).bytes;
        NativeHistorySelection selection;
        selection.resource = 1000;
        selection.sample = sample;
        NativePixelDrawHistoryLimits limits;
        limits.evaluateShaderOutputs = true;
        limits.evaluateOutputMerger = true;
        auto report = collectNativePixelDrawHistory(r, 100, selection, limits);
        verifyMergerJoin(report);
        QCOMPARE(report.at("records").size(), size_t(1));
        const auto &row = report.at("records")[0];
        QCOMPARE(row.at("private_shader_outputs").size(), size_t(1));
        const auto &out = row.at("private_shader_outputs")[0];
        QCOMPARE(out.at("resource_observation"), Json("current_native_bindings"));
        QCOMPARE(out.at("resource_boundary"), Json("before_private_primitive"));
        QCOMPARE(out.at("completed_primitives"), Json(0));
        QVERIFY(out.at("original_fragment_eligibility").is_null());
        QVERIFY(row.at("original_fragment_eligibility").is_null());
        QVERIFY(row.at("shader_output").is_null());
        const auto lane = out.at("selected_lane").get<size_t>();
        const auto &result = out.at("lanes")[lane];
        QCOMPARE(result.at("discarded"), Json(mode == "discard"));
        if (mode != "discard") {
            const auto &value = result.at("outputs")[0];
            QCOMPARE(value.at("valid_mask"), Json(15));
            if (mode != "masked")
                QCOMPARE((value.at("raw_bits").get<std::array<uint32_t, 4>>()), gpu);
            else {
                const std::array<float, 4> expected{.5f, .5f, .375f, .5f};
                std::array<uint32_t, 4> bits{};
                std::memcpy(bits.data(), expected.data(), 16);
                QCOMPARE((value.at("raw_bits").get<std::array<uint32_t, 4>>()), bits);
                QCOMPARE(row.at("observed_value_changed"), Json(false));
            }
        }
        if (mode == "constant" || mode == "coverage")
            QCOMPARE(out.at("input_provenance").at("coverage_only"), Json(true));
        if (mode == "sample")
            QCOMPARE(out.at("input_provenance").at("sample_frequency"), Json(true));
        if (mode == "derivative")
            QVERIFY(out.at("derivative_batches").get<uint64_t>() > 0);
        QCOMPARE(samples == 1 ? r.readTexture(1000) : r.readMsaa(1000, sample, 2).bytes, initial);
        std::array<uint32_t, 4> observed{};
        const auto after = report.at("after").at("bytes").get<std::vector<uint8_t>>();
        QCOMPARE(after.size(), size_t(16));
        std::memcpy(observed.data(), after.data(), 16);
        QCOMPARE(observed, gpu);
        report["reference_gpu_bits"] = gpu;
        report["mode"] = mode.toStdString();
        report["status"] = "ok";
        evidence(report);
    }
    void shaderHistoryRecovery_data() { devices(); }
    void shaderHistoryRecovery() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        auto c = blendCapture();
        drawCount(c, 100, 6);
        c.save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.shaders[10] = compileClassProgram(
            "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
            "vs_5_0");
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readTexture(1000);
        NativeHistorySelection select;
        select.resource = 1000;
        NativePixelDrawHistoryLimits limits;
        limits.evaluateShaderOutputs = true;
        limits.evaluateOutputMerger = true;
        auto first = collectNativePixelDrawHistory(r, 100, select, limits);
        verifyMergerJoin(first);
        QCOMPARE(first.at("shader_evaluation").at("invocations"), Json(2));
        const auto steps = first.at("shader_evaluation").at("global_steps").get<uint64_t>();
        QVERIFY(steps > 1);
        limits.maxShaderSteps = steps - 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, collectNativePixelDrawHistory(r, 100, select, limits));
        QCOMPARE(r.readTexture(1000), initial);
        limits.maxShaderSteps = 1000000;
        limits.maxShaderBytes = 1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, collectNativePixelDrawHistory(r, 100, select, limits));
        QCOMPARE(r.readTexture(1000), initial);
        limits.maxShaderBytes = 256ull * 1024 * 1024;
        bool cancelled = false;
        try {
            collectNativePixelDrawHistory(r, 100, select, limits, [] { return true; });
        } catch (const std::exception &) {
            cancelled = true;
        }
        QVERIFY(cancelled);
        QCOMPARE(r.readTexture(1000), initial);
        auto retry = collectNativePixelDrawHistory(r, 100, select, limits);
        verifyMergerJoin(retry);
        QCOMPARE(retry.at("after"), first.at("after"));
        QCOMPARE(retry.at("shader_evaluation"), first.at("shader_evaluation"));
        QCOMPARE(r.readTexture(1000), initial);
        retry["status"] = "ok";
        retry["failure_recovery"] = true;
        evidence(retry);
    }

    void drawHistory_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"ordinary", "blend", "depth", "stencil", "discard", "early_discard",
                                     "mask", "scissor", "cull", "clip", "unchanged", "depth_resource"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void drawHistory() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = depthStencilCapture();
        const Id event = mode == "unchanged" ? 2000 : 1000;
        drawCount(c, event, 6);
        auto o = options(vendor, event);
        o.shaders[30] = compileClassProgram(
            "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1," +
                std::string(mode == "clip" ? "-1" : ".25-v/3*.125") + ",1);}",
            "vs_5_0");
        auto depth = depthState();
        if (mode == "depth" || mode == "depth_resource") {
            depth.DepthEnable = TRUE;
            depth.DepthFunc = mode == "depth" ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_LESS;
        }
        if (mode == "stencil") {
            depth.StencilEnable = TRUE;
            depth.FrontFace.StencilFunc = depth.BackFace.StencilFunc = D3D11_COMPARISON_NEVER;
        }
        o.depthStencilEdits[1000].descriptor = o.depthStencilEdits[2000].descriptor = depth;
        if (mode == "mask")
            o.blendEdits[event].sampleMask = 0;
        if (mode == "blend") {
            D3D11_BLEND_DESC1 d{};
            auto &t = d.RenderTarget[0];
            t.BlendEnable = TRUE;
            t.SrcBlend = t.SrcBlendAlpha = t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ONE;
            t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            t.RenderTargetWriteMask = 15;
            o.blendEdits[event].descriptor = d;
            o.shaders[32] =
                compileClassProgram("float4 main():SV_Target{return float4(.25,.125,.375,.25);}", "ps_5_0");
        }
        if (mode == "discard" || mode == "early_discard")
            o.shaders[32] = compileClassProgram(
                std::string(mode == "early_discard" ? "[earlydepthstencil]" : "") +
                    "float4 main(float4 p:SV_Position,uint "
                    "id:SV_PrimitiveID):SV_Target{if(id==0)discard;return float4(p.xy,.25,1);}",
                "ps_5_0");
        if (mode == "scissor" || mode == "cull") {
            D3D11_RASTERIZER_DESC2 d{};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = mode == "cull" ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
            d.DepthClipEnable = TRUE;
            d.ScissorEnable = mode == "scissor";
            o.rasterizerEdits[event].descriptor = d;
            o.rasterizerEdits[event].scissors = std::vector<D3D11_RECT>{{1, 1, 2, 2}};
        }
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        NativeHistorySelection selection;
        selection.resource = mode == "depth_resource" ? 720 : 20;
        auto before = o;
        before.before = before.prepareBeforeDraw = true;
        Replay r(f, before);
        r.run();
        const auto initial = r.readTexture(selection.resource);
        const auto report = collectNativePixelDrawHistory(r, event, selection);
        QCOMPARE(r.readTexture(selection.resource), initial);
        QVERIFY(report.at("original_storage_restored").get<bool>());
        const bool noCandidates = mode == "scissor" || mode == "cull" || mode == "clip";
        QCOMPARE(report.at("records").size(), noCandidates ? size_t(0) : size_t(2));
        QCOMPARE(report.at("unattributed_changes").size(), size_t(0));
        Replay reference(f, o);
        reference.run();
        Json originalPrefixes = Json::array();
        QCOMPARE(report.at("after").at("bytes").get<std::vector<uint8_t>>(),
                 reference.readTexture(selection.resource));
        if (!noCandidates)
            for (uint32_t index = 0; index < 2; ++index) {
                const auto &row = report.at("records")[index];
                QCOMPARE(row.at("primitive_ordinal"), Json(index));
                QCOMPARE(row.at("original_primitive_id"), Json(index));
                QCOMPARE(row.at("record_kind"), Json("private_primitive_storage"));
                QVERIFY(row.at("write_executed").is_null());
                QVERIFY(row.at("original_fragment_eligibility").is_null());
                QVERIFY(row.at("failure_reasons").is_null());
                auto prefix = c;
                drawCount(prefix, event, (index + 1) * 3);
                const auto p = dir.filePath(QString("prefix%1.gpa_frame").arg(index));
                prefix.save(p);
                Frame pf(p.toStdWString());
                Replay originalPrefix(pf, o);
                originalPrefix.run();
                const auto prefixBytes = originalPrefix.readTexture(selection.resource);
                QCOMPARE(row.at("after").at("bytes").get<std::vector<uint8_t>>(), prefixBytes);
                originalPrefixes.push_back({{"completed_primitives", index + 1}, {"bytes", prefixBytes}});
                if (index)
                    QCOMPARE(row.at("before"), report.at("records")[index - 1].at("after"));
            }
        if (mode == "ordinary" || mode == "unchanged")
            QVERIFY(!report.at("records")[1].at("storage_changed").get<bool>());
        auto saved = report;
        saved["original_prefixes"] = originalPrefixes;
        saved["status"] = "ok";
        evidence(saved);
    }
    void drawHistoryMsaa_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("sample");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t sample = 0; sample < 4; ++sample)
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(sample))) << vendor << sample;
    }
    void drawHistoryMsaa() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, sample);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        blendCapture(1, false, false, 4).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.blendEdits[100].sampleMask = 5;
        o.shaders[12] = compileClassProgram("float4 main(float4 p:SV_Position,uint "
                                            "s:SV_SampleIndex):SV_Target{return float4(s*.25,p.xy,.5);}",
                                            "ps_5_0");
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readMsaa(1000, sample, 2).bytes;
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readMsaa(1000, sample, 2).bytes;
        NativeHistorySelection selection;
        selection.resource = 1000;
        selection.sample = sample;
        auto report = collectNativePixelDrawHistory(r, 100, selection);
        QCOMPARE(report.at("records").size(), size_t(1));
        QCOMPARE(report.at("after").at("bytes").get<std::vector<uint8_t>>(), expected);
        QCOMPARE(r.readMsaa(1000, sample, 2).bytes, initial);
        if (sample == 1 || sample == 3)
            QVERIFY(!report.at("records")[0].at("storage_changed").get<bool>());
        report["status"] = "ok";
        evidence(report);
    }
    void drawHistoryLayers_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("volume");
        QTest::addColumn<uint32_t>("layer");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool volume : {false, true})
                for (uint32_t layer : {0u, 1u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(volume).arg(layer)))
                        << vendor << volume << layer;
    }
    void drawHistoryLayers() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, volume);
        QFETCH(uint32_t, layer);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        layeredHistoryCapture(volume).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.shaders[10] = compileClassProgram(
            "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
            "vs_5_0");
        o.shaders[12] = compileClassProgram(
            "float4 main(float4 p:SV_Position):SV_Target{return float4(p.xy,.25,1);}", "ps_5_0");
        NativeHistorySelection selection;
        selection.resource = 1000;
        selection.mip = 1;
        selection.x = selection.y = 1;
        if (volume)
            selection.slice = layer;
        else
            selection.layer = layer + 1;
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readTexturePoint(1000, 1, 1, selection.slice, 1, selection.layer);
        QVERIFY(expected);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readTexture(1000);
        auto report = collectNativePixelDrawHistory(r, 100, selection);
        QCOMPARE(report.at("records").size(), size_t(1));
        QCOMPARE(report.at("records")[0].at("primitive_ordinal"), Json(layer));
        QVERIFY(report.at("records")[0].at("original_primitive_id").is_null());
        QCOMPARE(report.at("after").at("bytes").get<std::vector<uint8_t>>(), *expected);
        QCOMPARE(report.at("unattributed_changes").size(), size_t(0));
        QCOMPARE(r.readTexture(1000), initial);
        report["status"] = "ok";
        evidence(report);
    }
    void drawHistoryForced_data() { devices(); }
    void drawHistoryForced() {
        try {
            QFETCH(uint32_t, vendor);
            QTemporaryDir dir;
            const auto path = dir.filePath("frame.gpa_frame");
            blendCapture().save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor, 100);
            D3D11_RASTERIZER_DESC2 d{};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = D3D11_CULL_NONE;
            d.DepthClipEnable = TRUE;
            d.ForcedSampleCount = 4;
            o.rasterizerEdits[100].descriptor = d;
            o.depthStencilEdits[100].descriptor = depthState();
            Replay reference(f, o);
            reference.run();
            const auto expected = reference.readTexture(1000);
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            NativeHistorySelection selection;
            selection.resource = 1000;
            auto report = collectNativePixelDrawHistory(r, 100, selection);
            QCOMPARE(report.at("candidate_passes").size(), size_t(4));
            QCOMPARE(report.at("records").size(), size_t(1));
            QCOMPARE(report.at("records")[0].at("observations").size(), size_t(4));
            QCOMPARE(report.at("after").at("bytes").get<std::vector<uint8_t>>(), expected);
            report["status"] = "ok";
            evidence(report);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void drawHistoryRecovery_data() { devices(); }
    void drawHistoryRecovery() {
        QFETCH(uint32_t, vendor);
        auto c = depthStencilCapture();
        drawCount(c, 1000, 6);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 1000);
        o.before = o.prepareBeforeDraw = true;
        o.shaders[30] = compileClassProgram(
            "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
            "vs_5_0");
        Replay r(f, o);
        r.run();
        const auto initial = r.readTexture(20);
        NativeHistorySelection selection;
        selection.resource = 20;
        auto bad = selection;
        bad.mip = 1;
        QVERIFY_THROWS_EXCEPTION(std::exception, collectNativePixelDrawHistory(r, 1000, bad));
        bad = selection;
        bad.maxRecords = 1;
        QVERIFY_THROWS_EXCEPTION(std::exception, collectNativePixelDrawHistory(r, 1000, bad));
        NativePixelDrawHistoryLimits limit;
        limit.maxCandidates = 1;
        QVERIFY_THROWS_EXCEPTION(std::exception, collectNativePixelDrawHistory(r, 1000, selection, limit));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 collectNativePixelDrawHistory(r, 1000, selection, {}, [] { return true; }));
        QCOMPARE(r.readTexture(20), initial);
        auto report = collectNativePixelDrawHistory(r, 1000, selection);
        QCOMPARE(report.at("records").size(), size_t(2));
        QCOMPARE(r.readTexture(20), initial);
        report["status"] = "ok";
        evidence(report);
    }
    void checkpoints_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"ordinary", "depth", "depth_reject", "stencil", "stencil_reject",
                                     "discard", "early_discard", "blend", "color_mask", "sample_mask",
                                     "scissor", "inputs", "partial_position"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void checkpoints() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = depthStencilCapture();
        drawCount(c, 1000, 6);
        auto o = options(vendor, 1000);
        o.shaders[30] = compileClassProgram("float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return "
                                            "float4(i==2?3:-1,i==1?3:-1,.25-v/3*.125,1);}",
                                            "vs_5_0");
        auto depth = depthState();
        if (mode == "depth" || mode == "depth_reject") {
            depth.DepthEnable = TRUE;
            depth.DepthFunc = mode == "depth" ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_NEVER;
        }
        if (mode == "stencil" || mode == "stencil_reject") {
            depth.StencilEnable = TRUE;
            depth.FrontFace = depth.BackFace = {
                D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
                mode == "stencil" ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_NEVER};
        }
        o.depthStencilEdits[1000].descriptor = depth;
        if (mode == "sample_mask")
            o.blendEdits[1000].sampleMask = 0;
        if (mode == "blend" || mode == "color_mask") {
            D3D11_BLEND_DESC1 d{};
            for (auto &t : d.RenderTarget) {
                t.BlendEnable = mode == "blend";
                t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE;
                t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ONE;
                t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                t.RenderTargetWriteMask = mode == "color_mask" ? 0 : 15;
            }
            o.blendEdits[1000].descriptor = d;
            o.shaders[32] =
                compileClassProgram("float4 main():SV_Target{return float4(.25,.125,.375,.25);}", "ps_5_0");
        }
        if (mode == "scissor") {
            D3D11_RASTERIZER_DESC2 d{};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = D3D11_CULL_NONE;
            d.DepthClipEnable = d.ScissorEnable = TRUE;
            o.rasterizerEdits[1000].descriptor = d;
            o.rasterizerEdits[1000].scissors = std::vector<D3D11_RECT>{{1, 1, 2, 2}};
        }
        if (mode == "discard" || mode == "early_discard")
            o.shaders[32] = compileClassProgram(
                std::string(mode == "early_discard" ? "[earlydepthstencil]" : "") +
                    "float4 main(float4 p:SV_Position,uint "
                    "id:SV_PrimitiveID):SV_Target{if(id==0)discard;return float4(p.x,p.y,.25,1);}",
                "ps_5_0");
        if (mode == "inputs") {
            const std::string fields = "struct V{float4 p:SV_Position;float2 a:TEXCOORD0;float "
                                       "b:TEXCOORD1;nointerpolation uint c:TEXCOORD2;};";
            o.shaders[30] = compileClassProgram(
                fields +
                    "V main(uint v:SV_VertexID){uint i=v%3;V "
                    "o;o.p=float4(i==2?3:-1,i==1?3:-1,.25,1);o.a=float2(i,.5);o.b=.25;o.c=v/3;return o;}",
                "vs_5_0");
            o.shaders[32] = compileClassProgram(
                fields + "float4 main(V v,uint id:SV_PrimitiveID,bool front:SV_IsFrontFace):SV_Target{return "
                         "float4(v.a.x,v.b,v.c*.25+id*.125,front?1:0);}",
                "ps_5_0");
        }
        if (mode == "partial_position")
            o.shaders[32] =
                compileClassProgram("float4 main(float4 p:SV_Position):SV_Target{return p.x;}", "ps_5_0");
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto before = o;
        before.before = before.prepareBeforeDraw = true;
        Replay r(f, before);
        r.run();
        const auto originalColor = r.readTexture(20), originalDepth = r.readTexture(720);
        std::vector<std::vector<uint8_t>> colors, depths;
        auto report =
            replayNativePixelGeometry(r, 1000, {0, 2, {1}}, [&](Replay &privateReplay, uint64_t completed) {
                if (completed != colors.size())
                    throw std::runtime_error("Unexpected checkpoint order");
                colors.push_back(privateReplay.readTexture(20));
                depths.push_back(privateReplay.readTexture(720));
            });
        QCOMPARE(colors.size(), size_t(3));
        QCOMPARE(colors[0], originalColor);
        QCOMPARE(depths[0], originalDepth);
        QCOMPARE(r.readTexture(20), originalColor);
        QCOMPARE(r.readTexture(720), originalDepth);
        Json references = Json::array();
        for (uint32_t completed = 1; completed <= 2; ++completed) {
            auto prefix = c;
            drawCount(prefix, 1000, completed * 3);
            const auto p = dir.filePath(QString("prefix%1.gpa_frame").arg(completed));
            prefix.save(p);
            Frame pf(p.toStdWString());
            auto measured = o;
            measured.measurement = ReplayOptions::Measurement{1000, 1000, true};
            Replay reference(pf, measured);
            reference.run();
            QCOMPARE(colors[completed], reference.readTexture(20));
            QCOMPARE(depths[completed], reference.readTexture(720));
            QVERIFY(reference.measurementResult());
            uint64_t cumulative = 0;
            for (uint32_t i = 0; i < completed; ++i)
                cumulative += report.at("batches")[i].at("passing_samples").get<uint64_t>();
            QCOMPARE(cumulative, reference.measurementResult()->occlusion);
            references.push_back({{"completed", completed},
                                  {"passing_samples", reference.measurementResult()->occlusion},
                                  {"color_sha256", sha256(colors[completed])},
                                  {"depth_sha256", sha256(depths[completed])}});
        }
        const auto repeat = replayNativePixelGeometry(r, 1000, {0, 2, {1}});
        QCOMPARE(repeat.at("batches"), report.at("batches"));
        QVERIFY(report.at("original_fragment_eligibility").is_null());
        QCOMPARE(report.at("extra_uav_slots"), Json(0));
        report["reference_prefixes"] = references;
        report["status"] = "ok";
        evidence(report);
    }
    void msaa_data() { devices(); }
    void msaa() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        blendCapture(1, false, false, 4).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.shaders[12] =
            compileClassProgram("float4 main(float4 p:SV_Position,uint "
                                "sample:SV_SampleIndex):SV_Target{return float4(sample*.25,p.xy,.5);}",
                                "ps_5_0");
        o.blendEdits[100].sampleMask = 5;
        Replay reference(f, o);
        reference.run();
        auto samples = [](Replay &p) {
            std::vector<uint8_t> result;
            for (uint32_t sample = 0; sample < 4; ++sample) {
                const auto bytes = p.readMsaa(1000, sample, 2).bytes;
                result.insert(result.end(), bytes.begin(), bytes.end());
            }
            return result;
        };
        const auto expected = samples(reference);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto original = samples(r);
        std::vector<uint8_t> actual;
        auto report = replayNativePixelGeometry(r, 100, {0, 1}, [&](Replay &p, uint64_t n) {
            if (n)
                actual = samples(p);
        });
        QCOMPARE(actual, expected);
        QCOMPARE(samples(r), original);
        report["output_sha256"] = sha256(actual);
        report["status"] = "ok";
        evidence(report);
    }
    void bindings_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"append", "counter", "classes", "predicate_pass", "predicate_skip"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void bindings() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        const bool predicated = mode.startsWith("predicate");
        const bool counter = mode == "append" || mode == "counter";
        auto c = predicated          ? predicateCapture(true, mode == "predicate_skip" ? 1 : 0)
                 : mode == "classes" ? graphicsClassCapture()
                                     : graphicsCounterCapture(mode == "counter");
        const Id event = predicated ? 2000 : 200;
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, event);
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readTexture(20);
        const auto expectedBuffer = counter ? reference.readBuffer(10) : std::vector<uint8_t>{};
        const auto expectedCounter = counter ? reference.readCounter(12) : 0;
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto original = r.readTexture(20);
        const auto originalBuffer = counter ? r.readBuffer(10) : std::vector<uint8_t>{};
        const auto originalCounter = counter ? r.readCounter(12) : 0;
        Com<ID3D11PixelShader> ps;
        std::array<ID3D11ClassInstance *, 256> rawClasses{};
        std::array<Com<ID3D11ClassInstance>, 256> classes;
        UINT classCount = 256;
        context(r)->PSGetShader(&ps, rawClasses.data(), &classCount);
        for (UINT i = 0; i < classCount; ++i)
            classes[i].Attach(rawClasses[i]);
        std::vector<uint8_t> actual, actualBuffer;
        uint32_t actualCounter = 0;
        auto report = replayNativePixelGeometry(
            r, event, {0, mode == "predicate_skip" ? 0u : 1u}, [&](Replay &p, uint64_t) {
                Com<ID3D11PixelShader> bound;
                std::array<ID3D11ClassInstance *, 256> observed{};
                UINT count = 256;
                context(p)->PSGetShader(&bound, observed.data(), &count);
                bool matched = bound == ps && count == classCount;
                for (UINT i = 0; i < count; ++i) {
                    matched &= observed[i] == rawClasses[i];
                    observed[i]->Release();
                }
                if (!matched)
                    throw std::runtime_error("Original PS or class bindings changed");
                actual = p.readTexture(20);
                if (counter) {
                    actualBuffer = p.readBuffer(10);
                    actualCounter = p.readCounter(12);
                }
            });
        QCOMPARE(actual, expected);
        QCOMPARE(r.readTexture(20), original);
        if (counter) {
            QCOMPARE(actualBuffer, expectedBuffer);
            QCOMPARE(actualCounter, expectedCounter);
            QCOMPARE(r.readBuffer(10), originalBuffer);
            QCOMPARE(r.readCounter(12), originalCounter);
            QCOMPARE(actualCounter, originalCounter + 1);
        }
        // Re-execute the original draw on the restored native bindings.
        context(r)->Draw(3, 0);
        QCOMPARE(r.readTexture(20), expected);
        if (counter) {
            QCOMPARE(r.readBuffer(10), expectedBuffer);
            QCOMPARE(r.readCounter(12), expectedCounter);
        }
        report["status"] = "ok";
        report["original_class_count"] = classCount;
        report["counter_before"] = originalCounter;
        report["counter_after_private"] = actualCounter;
        evidence(report);
    }
    void streamOutput_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("dirty");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool dirty : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(dirty))) << vendor << dirty;
    }
    void streamOutput() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, dirty);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        streamCapture(false, dirty).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 150);
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readBuffer(70);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readBuffer(70);
        auto report = replayNativePixelGeometry(r, 150, {0, 1});
        QCOMPARE(r.readBuffer(70), initial);
        context(r)->Draw(1, 0);
        QCOMPARE(r.readBuffer(70), expected);
        report["status"] = "ok";
        report["restored_so_sha256"] = sha256(expected);
        evidence(report);
    }
    void selections_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"null_ps", "disabled", "empty", "subset"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void selections() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = depthStencilCapture();
        auto o = options(vendor, 1000);
        NativePixelGeometryRange range{0, 1};
        if (mode == "null_ps") {
            c.entries.erase(
                std::remove_if(c.entries.begin(), c.entries.end(), [](const auto &e) { return e.id == 990; }),
                c.entries.end());
            State s{};
            s.topology = 4;
            s.sampleMask = UINT32_MAX;
            s.omStart = 8;
            s.dsv = 722;
            s.viewports = 22;
            s.rasterizer = 724;
            s.stages[0].shader = 30;
            c.add(990, 3, 3, snapshot(s));
        }
        if (mode == "disabled") {
            o.disabled.insert(1000);
            range.primitiveCount = 0;
        }
        if (mode == "empty") {
            drawCount(c, 1000, 0);
            range.primitiveCount = 0;
        }
        if (mode == "subset") {
            drawCount(c, 1000, 6);
            range.firstPrimitive = 1;
            o.shaders[30] = compileClassProgram("float4 main(uint v:SV_VertexID):SV_Position{uint "
                                                "i=v%3;return float4(i==2?3:-1,i==1?3:-1,.25,1);}",
                                                "vs_5_0");
            o.shaders[32] =
                compileClassProgram("float4 main(float4 p:SV_Position,uint "
                                    "id:SV_PrimitiveID):SV_Target{return float4((id+1)*.25,0,0,1);}",
                                    "ps_5_0");
            o.depthStencilEdits[1000].descriptor = depthState();
        }
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        Replay reference(f, o);
        reference.run();
        const auto expectedColor = reference.readTexture(20), expectedDepth = reference.readTexture(720);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto beforeColor = r.readTexture(20), beforeDepth = r.readTexture(720);
        std::vector<uint8_t> color, depth;
        auto report = replayNativePixelGeometry(r, 1000, range, [&](Replay &p, uint64_t) {
            color = p.readTexture(20);
            depth = p.readTexture(720);
        });
        QCOMPARE(color, expectedColor);
        QCOMPARE(depth, expectedDepth);
        QCOMPARE(r.readTexture(20), beforeColor);
        QCOMPARE(r.readTexture(720), beforeDepth);
        report["status"] = "ok";
        report["output_sha256"] = sha256(color);
        report["depth_sha256"] = sha256(depth);
        evidence(report);
    }
    void recovery_data() { devices(); }
    void recovery() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        blendCapture().save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readTexture(1000);
        bool cancelled = false;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 replayNativePixelGeometry(
                                     r, 100, {0, 1}, [&](Replay &, uint64_t n) { cancelled = n != 0; },
                                     [&] { return cancelled; }));
        QCOMPARE(r.readTexture(1000), initial);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 replayNativePixelGeometry(r, 100, {0, 1}, [](Replay &, uint64_t) {
                                     throw std::runtime_error("Observer failure");
                                 }));
        QCOMPARE(r.readTexture(1000), initial);
        for (auto selection : std::vector<NativePixelGeometryRange>{{1, 1},
                                                                    {0, 2},
                                                                    {0, 1, {1, 1}},
                                                                    {0, 1, {2}},
                                                                    {0, 1, {}, 1},
                                                                    {0, 1, {}, 256ull * 1024 * 1024, 60001}})
            QVERIFY_THROWS_EXCEPTION(std::exception, replayNativePixelGeometry(r, 100, selection));
        try {
            replayNativePixelGeometry(r, 100, {0, 1, {}, 256ull * 1024 * 1024, 20},
                                      [](Replay &, uint64_t) { QTest::qSleep(30); });
            QFAIL("Expired diagnostic deadline was accepted");
        } catch (const OperationCancelled &) {
            QFAIL("Deadline was reported as user cancellation");
        } catch (const std::runtime_error &error) {
            QCOMPARE(std::string(error.what()), std::string("Native pixel geometry replay deadline"));
        }
        std::vector<uint8_t> result;
        auto report = replayNativePixelGeometry(r, 100, {0, 1}, [&](Replay &p, uint64_t n) {
            if (n)
                result = p.readTexture(1000);
        });
        QCOMPARE(r.readTexture(1000), initial);
        o.before = o.prepareBeforeDraw = false;
        Replay reference(f, o);
        reference.run();
        QCOMPARE(result, reference.readTexture(1000));
        report["status"] = "ok";
        report["recovered_failures"] = 9;
        evidence(report);
    }
};
QTEST_GUILESS_MAIN(NativePixelGeometryReplayTests)
#include "NativePixelGeometryReplayTests.moc"
