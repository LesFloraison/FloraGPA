#include "BlendCapture.h"
#include "application/NativePixelGeometryReplay.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
std::array<uint32_t, 4> pixel(Replay &r, uint32_t sample, uint32_t samples, uint32_t format) {
    const auto bytes = samples == 1 ? r.readTexture(1000) : r.readMsaa(1000, sample, format).bytes;
    if (bytes.size() != 16)
        throw std::runtime_error("Unexpected primitive mask pixel extent");
    std::array<uint32_t, 4> result{};
    std::memcpy(result.data(), bytes.data(), 16);
    return result;
}
void save(const Json &report, Bytes original, const std::vector<std::vector<uint8_t>> &shaders) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_PRIMITIVE_MASK_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create primitive mask evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto text = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot write primitive mask evidence");
    auto binary = [&](const QString &suffix, Bytes bytes) {
        QFile output(file.fileName() + suffix);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            output.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
                qint64(bytes.size()))
            throw std::runtime_error("Cannot write primitive mask shader evidence");
    };
    binary(".original.dxbc", original);
    for (size_t i = 0; i < shaders.size(); ++i)
        binary(QString(".%1.dxbc").arg(i), shaders[i]);
}
} // namespace
class DxbcPixelPrimitiveMaskTests final : public QObject {
    Q_OBJECT
  private slots:
    void draw_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"added", "existing", "discard", "early-discard", "integer", "sample",
                                     "upper-gs", "added-gs", "packed"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void draw() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            const bool integer = mode == "integer", gs = mode.endsWith("gs");
            const uint32_t samples = mode == "sample" ? 4 : 1, format = integer ? 3 : 2;
            auto c = blendCapture(1, false, integer, samples);
            for (const auto &entry : c.entries)
                if (entry.id == 100)
                    put(c.bytes, size_t(entry.offset) + 24, 9u);
            if (gs) {
                const auto code = compileClassProgram(
                    "struct V{float4 p:SV_Position;};struct O{float4 p:SV_Position;uint id:SV_PrimitiveID;};"
                    "[maxvertexcount(3)]void main(triangle V v[3],uint id:SV_PrimitiveID,inout "
                    "TriangleStream<O> s){"
                    "[unroll]for(uint i=0;i<3;i++){O o;o.p=v[i].p;o.id=id|0xfffffffcu;s.Append(o);}}",
                    "gs_5_0");
                c.add(60, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(61)));
                auto raw = statePack(uint64_t(code.size()));
                raw.insert(raw.end(), code.begin(), code.end());
                append(raw, Id(0));
                c.add(61, 9, 0x81, raw);
                std::erase_if(c.entries, [](const auto &e) { return e.id == 90 || e.id == 190; });
                State state{};
                state.topology = 4;
                state.sampleMask = UINT32_MAX;
                state.omStart = 8;
                state.rtCount = 1;
                state.blend = 64;
                state.rasterizer = 52;
                state.viewports = 50;
                state.stages[0].shader = 10;
                state.stages[3].shader = 60;
                state.stages[4].shader = 12;
                state.rtv[0] = 1002;
                c.add(90, 3, 3, snapshot(state));
                c.add(190, 3, 3, snapshot(state));
            }
            std::string source = mode == "early-discard" ? "[earlydepthstencil]" : "";
            source += integer ? "uint4 main(" : "float4 main(";
            if (gs)
                source += "float4 p:SV_Position";
            if (mode == "upper-gs")
                source += ",";
            if (mode != "added" && mode != "added-gs")
                source += "uint id:SV_PrimitiveID";
            if (samples > 1)
                source += ",uint sample:SV_SampleIndex";
            if (mode == "packed")
                source += ",bool front:SV_IsFrontFace";
            source += "):SV_Target{";
            if (mode.contains("discard"))
                source += "if(id==2)discard;";
            source += integer              ? "return uint4(id,5,6,7);}"
                      : mode == "added"    ? "return float4(.25,.5,.75,.5);}"
                      : mode == "added-gs" ? "return float4(p.xy,.75,.5);}"
                      : mode == "upper-gs" ? "return float4(id,p.xy,.5);}"
                      : mode == "packed"   ? "return float4(id,front,0,.5);}"
                      : samples > 1        ? "return float4(id,sample,0,.5);}"
                                           : "return float4(id,0,0,.5);}";
            const auto original = compileClassProgram(source, "ps_5_0");
            QTemporaryDir dir;
            const auto path = dir.filePath("frame.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            ReplayOptions o;
            o.debug = true;
            o.warp = !vendor;
            if (vendor)
                o.vendor = vendor;
            o.until = 100;
            o.shaders[10] = compileClassProgram("float4 main(uint v:SV_VertexID):SV_Position{uint "
                                                "i=v%3;return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                                                "vs_5_0");
            o.shaders[12] = original;
            Replay reference(f, o);
            reference.run();
            std::vector<std::array<uint32_t, 4>> expected;
            for (uint32_t sample = 0; sample < samples; ++sample)
                expected.push_back(pixel(reference, sample, samples, format));
            o.before = o.prepareBeforeDraw = true;
            Replay r(f, o);
            r.run();
            std::vector<std::array<uint32_t, 4>> before;
            for (uint32_t sample = 0; sample < samples; ++sample)
                before.push_back(pixel(r, sample, samples, format));
            const uint32_t last = gs ? 0xfffffffeu : mode.contains("discard") ? 1u : 2u;
            Json rows = Json::array();
            std::vector<std::vector<uint8_t>> shaders;
            for (uint32_t bit = 0; bit < 34; ++bit) {
                const uint32_t mask = bit < 32 ? 1u << bit : UINT32_MAX;
                const uint32_t value = bit < 32 ? mask : bit == 32 ? last : last ^ 1;
                std::vector<std::array<uint32_t, 4>> values;
                auto report =
                    replayNativePixelPrimitiveMask(r, 100, 0, {mask, value}, [&](Replay &p, bool after) {
                        if (after)
                            for (uint32_t sample = 0; sample < samples; ++sample)
                                values.push_back(pixel(p, sample, samples, format));
                    });
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const auto marked =
                        report.at("completion_shader").at("marker_bits").get<std::array<uint32_t, 3>>();
                    for (uint32_t i = 0; i < 3; ++i)
                        QCOMPARE(values[sample][i], ((last & mask) == value) ? marked[i] : 0u);
                    QCOMPARE(values[sample][3], expected[sample][3]);
                    QCOMPARE(pixel(r, sample, samples, format), before[sample]);
                }
                QCOMPARE(report.at("primitive_id_source"), Json(gs ? "bound_geometry_shader" : "rasterizer"));
                report["pixels"] = values;
                std::optional<PixelPrimitiveInput> producer;
                if (gs)
                    producer = PixelPrimitiveInput{
                        report.at("completion_shader").at("input_register").get<uint32_t>(),
                        report.at("completion_shader").at("input_component").get<uint32_t>()};
                auto rewritten = instrumentPixelPrimitiveMask(original, 0, {mask, value},
                                                              256ull * 1024 * 1024, {}, producer);
                QCOMPARE(rewritten.identity, report.at("completion_shader"));
                shaders.push_back(std::move(rewritten.bytecode));
                rows.push_back(std::move(report));
            }
            save({{"schema", 1},
                  {"status", "ok"},
                  {"vendor_request", vendor},
                  {"mode", mode.toStdString()},
                  {"expected_system_primitive_id", last},
                  {"reference", expected},
                  {"samples", samples},
                  {"observations", rows},
                  {"original_storage_restored", true},
                  {"original_fragment_eligibility", nullptr}},
                 original, shaders);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void boundaries() {
        const auto original = compileClassProgram("float4 main():SV_Target{return 1;}", "ps_5_0");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, instrumentPixelPrimitiveMask(original, 0, {0, 0}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, instrumentPixelPrimitiveMask(original, 0, {1, 2}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 instrumentPixelPrimitiveMask(original, 0, {1, 1}, original.size()));
        QVERIFY_THROWS_EXCEPTION(
            OperationCancelled,
            instrumentPixelPrimitiveMask(original, 0, {1, 1}, 256ull * 1024 * 1024, [] { return true; }));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 instrumentPixelPrimitiveMask(original, 0, {1, 1}, 256ull * 1024 * 1024, {},
                                                              PixelPrimitiveInput{32, 0}));
        const auto existing = compileClassProgram(
            "float4 main(uint id:SV_PrimitiveID):SV_Target{return float4(id,0,0,1);}", "ps_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 instrumentPixelPrimitiveMask(existing, 0, {1, 1}, 256ull * 1024 * 1024, {},
                                                              PixelPrimitiveInput{31, 0}));
    }
};
QTEST_GUILESS_MAIN(DxbcPixelPrimitiveMaskTests)
#include "DxbcPixelPrimitiveMaskTests.moc"
