#include "DepthStencilCapture.h"
#include "application/NativePixelDrawHistory.h"
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
void evidence(const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_SAMPLE_DEPTH_EVIDENCE");
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
} // namespace
class NativePixelSampleDepthTests final : public QObject {
    Q_OBJECT
  private slots:
    void sampleDepthHistory_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("sample");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 4u})
                for (uint32_t sample = 0; sample < samples; ++sample)
                    for (uint32_t format : {20u, 40u, 45u, 55u})
                        for (const auto *mode : {"flat", "slope", "bias", "clamp", "color-bias"})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(samples)
                                                         .arg(sample)
                                                         .arg(format)
                                                         .arg(mode)))
                                << vendor << samples << sample << format << QString(mode);
    }
    void sampleDepthHistory() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, samples);
        QFETCH(uint32_t, sample);
        QFETCH(uint32_t, format);
        QFETCH(QString, mode);
        const bool color = mode == "color-bias";
        auto capture = depthStencilCapture();
        for (const auto &entry : capture.entries) {
            const auto offset = size_t(entry.offset);
            if (entry.id == 720) {
                put(capture.bytes, offset + 16, 8u);
                put(capture.bytes, offset + 20, 8u);
                put(capture.bytes, offset + 32, format);
                put(capture.bytes, offset + 36, samples);
            }
            if (entry.id == 722) {
                put(capture.bytes, offset + 24, format);
                put(capture.bytes, offset + 28, samples > 1 ? 5u : 3u);
            }
            if (entry.id == 810)
                put(capture.bytes, offset + 24, format == 20 || format == 45 ? 3u : 1u);
            if (entry.id == 20 && color) {
                put(capture.bytes, offset + 16, 8u);
                put(capture.bytes, offset + 20, 8u);
                put(capture.bytes, offset + 32, 3u);
                put(capture.bytes, offset + 36, samples);
            }
            if (entry.id == 21 && color) {
                put(capture.bytes, offset + 24, 3u);
                put(capture.bytes, offset + 28, samples > 1 ? 6u : 4u);
            }
            if (entry.id == 22) {
                put(capture.bytes, offset + 12, 8.f);
                put(capture.bytes, offset + 16, 8.f);
            }
        }
        std::erase_if(capture.entries, [](const auto &e) { return e.id == 990; });
        State bound{};
        bound.topology = 4;
        bound.sampleMask = UINT32_MAX;
        bound.omStart = 8;
        bound.dsv = 722;
        if (color) {
            bound.rtCount = 1;
            bound.rtv[0] = 21;
        }
        bound.rasterizer = 724;
        bound.viewports = 22;
        bound.stages[0].shader = 30;
        bound.stages[4].shader = 32;
        capture.add(990, 3, 3, snapshot(bound));
        auto o = options(vendor, 1000);
        o.shaders[30] = compileClassProgram(
            std::string("float4 main(uint i:SV_VertexID):SV_Position{float2 p=float2(i==2?3:-1,i==1?3:-1);") +
                "return float4(p," + (mode == "flat" ? ".25" : ".125+(p.x+1)*.125") + ",1);}",
            "vs_5_0");
        o.shaders[32] = compileClassProgram(
            color ? "uint4 main(float4 p:SV_Position):SV_Target{return uint4(asuint(p.z),0,0,1);}"
                  : "void main(){}",
            "ps_5_0");
        auto depth = depthState();
        depth.DepthEnable = TRUE;
        o.depthStencilEdits[1000].descriptor = depth;
        D3D11_RASTERIZER_DESC2 raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.MultisampleEnable = samples > 1;
        if (mode == "bias" || mode == "clamp" || color) {
            raster.DepthBias = 4;
            raster.SlopeScaledDepthBias = .25f;
            raster.DepthBiasClamp = mode == "clamp" ? .001f : 0.f;
        }
        o.rasterizerEdits[1000].descriptor = raster;
        QTemporaryDir directory;
        const auto path = directory.filePath("depth.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        Replay reference(frame, o);
        reference.run();
        auto texel = [&](Id id, uint32_t fmt) {
            const auto storage = samples > 1
                                     ? reference.observeMsaaStorage(id, sample, fmt, 1024 * 1024).bytes
                                     : reference.observeTextureStorage(id, 1024 * 1024).bytes;
            const auto size = pitches(1, 1, fmt).first;
            const size_t offset = (2 * 8 + 3) * size;
            return std::vector<uint8_t>(storage.begin() + offset, storage.begin() + offset + size);
        };
        const auto expectedDepth = texel(720, format);
        const auto expected = color ? texel(20, 3) : expectedDepth;
        o.before = o.prepareBeforeDraw = true;
        Replay replay(frame, o);
        replay.run();
        NativeHistorySelection selection;
        selection.resource = color ? 20 : 720;
        selection.x = 3;
        selection.y = 2;
        selection.sample = sample;
        NativePixelDrawHistoryLimits limits;
        limits.evaluateShaderOutputs = limits.evaluateOutputMerger = true;
        auto report = collectNativePixelDrawHistory(replay, 1000, selection, limits);
        report["mode"] = mode.toStdString();
        report["reference_depth_bytes"] = expectedDepth;
        report["reference_selected_bytes"] = expected;
        evidence(report);
        for (size_t i = 0; i < expected.size(); ++i)
            if (report.at("after").at("valid_bytes")[i] == true)
                QCOMPARE(report.at("after").at("bytes")[i], Json(expected[i]));
        const auto &merger = report.at("records")[0].at("private_output_mergers")[0].at("merger");
        for (size_t i = 0; i < expectedDepth.size(); ++i)
            if (format != 20 || i < 5)
                QCOMPARE(merger.at("depth_after")[sample * expectedDepth.size() + i], Json(expectedDepth[i]));
        verifyMergerJoin(report);
    }
};
QTEST_GUILESS_MAIN(NativePixelSampleDepthTests)
#include "NativePixelSampleDepthTests.moc"
