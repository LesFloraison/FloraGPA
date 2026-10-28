#include "BlendCapture.h"
#include "application/NativePixelDrawHistory.h"
#include "application/NativePixelGeometryReplay.h"
#include "application/NativePixelIdentity.h"
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
    binary(".reference.dxbc",
           instrumentPixelCompletion(original, 0, {0x3f800001, 0x40000001, 0x40400001}).bytecode);
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
                                     "upper-gs", "added-gs", "duplicate-gs", "packed"})
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
                    std::string("struct V{float4 p:SV_Position;};struct O{float4 p:SV_Position;uint "
                                "id:SV_PrimitiveID;};"
                                "[maxvertexcount(3)]void main(triangle V v[3],uint id:SV_PrimitiveID,inout "
                                "TriangleStream<O> s){"
                                "[unroll]for(uint i=0;i<3;i++){O o;o.p=v[i].p;o.id=") +
                        (mode == "duplicate-gs" ? "7u" : "id|0xfffffffcu") + ";s.Append(o);}}",
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
            if (gs && mode != "added-gs")
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
                      : gs                 ? "return float4(id,p.xy,.5);}"
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
            const uint32_t last = mode == "duplicate-gs"     ? 7u
                                  : gs                       ? 0xfffffffeu
                                  : mode.contains("discard") ? 1u
                                                             : 2u;
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
            Json histories = Json::array();
            for (uint32_t sample = 0; sample < samples; ++sample) {
                NativeHistorySelection selection;
                selection.resource = 1000;
                selection.sample = sample;
                NativePixelDrawHistoryLimits limits;
                limits.observePrimitiveIdentity = true;
                auto history = collectNativePixelDrawHistory(r, 100, selection, limits);
                const auto &identity = history.at("primitive_identity_observation");
                QCOMPARE(identity.at("status"), Json("verified_conditional_replay"));
                QCOMPARE(identity.at("system_primitive_id"), Json(last));
                QCOMPARE(identity.at("pass_count"), Json(36));
                QCOMPARE(history.at("primitive_identity_association").at("matching_primitive_ordinals"),
                         mode == "duplicate-gs" ? Json::array({0, 1, 2})
                                                : Json::array({mode.contains("discard") ? 1 : 2}));
                QCOMPARE(history.at("primitive_identity_association").at("status"),
                         Json(mode == "duplicate-gs" ? "ambiguous_candidates" : "one_matching_candidate"));
                QCOMPARE(pixel(r, sample, samples, format), before[sample]);
                histories.push_back(std::move(history));
            }
            Json recovery = nullptr;
            if (mode == "added") {
                const auto reader = [&](Replay &p) {
                    return NativePixelPredicateStorage{format, p.readTexture(1000)};
                };
                unsigned reads = 0;
                QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                         observeNativePixelIdentity(
                                             r, 100, 0,
                                             [&](Replay &p) {
                                                 ++reads;
                                                 return reader(p);
                                             },
                                             256ull * 1024 * 1024, 30000, [&] { return reads >= 5; }));
                QCOMPARE(pixel(r, 0, 1, format), before[0]);
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         observeNativePixelIdentity(r, 100, 0, [&](Replay &p) {
                                             auto value = reader(p);
                                             value.format = 4;
                                             return value;
                                         }));
                QCOMPARE(pixel(r, 0, 1, format), before[0]);
                auto retried = observeNativePixelIdentity(r, 100, 0, reader);
                QCOMPARE(retried.at("system_primitive_id"), Json(last));
                QCOMPARE(pixel(r, 0, 1, format), before[0]);
                unsigned faultReads = 0;
                auto injected = observeNativePixelIdentity(r, 100, 0, [&](Replay &p) {
                    auto value = reader(p);
                    if (++faultReads == 68)
                        std::fill(value.bytes.begin(), value.bytes.begin() + 12, uint8_t(0));
                    return value;
                });
                QCOMPARE(injected.at("status"), Json("inconsistent"));
                QVERIFY(injected.at("system_primitive_id").is_null());
                QCOMPARE(injected.at("candidate_system_id"), Json(last));
                QCOMPARE(pixel(r, 0, 1, format), before[0]);
                o.blendEdits[100].sampleMask = 0;
                Replay masked(f, o);
                masked.run();
                auto absent = observeNativePixelIdentity(masked, 100, 0, reader);
                QCOMPARE(absent.at("status"), Json("no_color_completion"));
                QCOMPARE(absent.at("pass_count"), Json(1));
                QVERIFY(absent.at("system_primitive_id").is_null());
                QCOMPARE(pixel(masked, 0, 1, format), before[0]);
                recovery = {{"cancelled_after_reads", reads},
                            {"wrong_format_rejected", true},
                            {"retry", retried},
                            {"injected_equal_readback_zero", true},
                            {"fault_injection", injected},
                            {"masked", absent},
                            {"storage_restored", true}};
            }
            save({{"schema", 1},
                  {"status", "ok"},
                  {"vendor_request", vendor},
                  {"mode", mode.toStdString()},
                  {"expected_system_primitive_id", last},
                  {"reference", expected},
                  {"samples", samples},
                  {"observations", rows},
                  {"histories", histories},
                  {"identity_recovery", recovery},
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
    void predicateDecode() {
        struct Case {
            uint32_t format;
            std::vector<uint8_t> bytes;
            Json expected;
        };
        std::vector<Case> cases{
            {2, statePack(1.f, 2.f, 3.f, .5f), true},
            {2, statePack(0.f, 0.f, 0.f, 1.f), false},
            {2, statePack(0.f, 1.f, 0.f, 1.f), nullptr},
            {2, statePack(0x7fc00001u, 0x3f800000u, 0x3f800000u, 0u), nullptr},
            {2, statePack(-1.f, 2.f, 3.f, .5f), nullptr},
            {3, statePack(1u, 2u, 3u, 7u), true},
            {4, statePack(1, 2, 3, 7), true},
            {4, statePack(-1, 2, 3, 7), nullptr},
            {10, statePack(uint16_t(0x3c00), uint16_t(0x4000), uint16_t(0x4200), uint16_t(0x3800)), true},
            {11, statePack(uint16_t(1), uint16_t(2), uint16_t(3), uint16_t(65535)), true},
            {12, statePack(uint16_t(1), uint16_t(2), uint16_t(3), uint16_t(7)), true},
            {13, statePack(uint16_t(1), uint16_t(2), uint16_t(3), uint16_t(32767)), true},
            {14, statePack(uint16_t(1), uint16_t(2), uint16_t(3), uint16_t(7)), true},
            {16, statePack(1.f, 2.f), true},
            {17, statePack(1u, 2u), true},
            {24, statePack(1u | (2u << 10) | (3u << 20) | (3u << 30)), true},
            {24, statePack(3u << 30), false},
            {25, statePack(1u | (2u << 10) | (3u << 20) | (3u << 30)), true},
            {26, statePack(0x3c0u | (0x3c0u << 11) | (0x1e0u << 22)), true},
            {28, {1, 2, 3, 255}, true},
            {28, {0, 0, 0, 255}, false},
            {29, {1, 2, 3, 255}, true},
            {30, {1, 2, 3, 255}, true},
            {31, {1, 2, 3, 127}, true},
            {32, {1, 2, 3, 127}, true},
            {41, statePack(1.f), true},
            {42, statePack(1u), true},
            {43, statePack(-1), nullptr},
            {49, {1, 2}, true},
            {50, {1, 2}, true},
            {61, {1}, true},
            {62, {1}, true},
            {63, {255}, nullptr},
            {64, {1}, true},
            {65, {255}, nullptr},
            {87, {1, 2, 3, 255}, true},
            {88, {0, 0, 0, 255}, false},
            {91, {1, 2, 3, 255}, true},
            {1, std::vector<uint8_t>(16), nullptr}};
        Json evidence = Json::array();
        for (const auto &item : cases) {
            const auto decoded = decodeNativePixelPredicate({item.format, item.bytes});
            QCOMPARE(decoded.at("rgb_predicate"), item.expected);
            evidence.push_back(decoded);
            const auto zero =
                decodeNativePixelPredicate({item.format, std::vector<uint8_t>(item.bytes.size())});
            QCOMPARE(zero.at("rgb_predicate"),
                     item.format == 1 || item.format == 65 ? Json(nullptr) : Json(false));
            evidence.push_back(zero);
        }
        const auto root = qEnvironmentVariable("FLORA_PIXEL_PRIMITIVE_MASK_EVIDENCE");
        if (!root.isEmpty()) {
            QFile file(root + ".decode.json");
            const auto text = evidence.dump(2);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(file.write(text.data(), qint64(text.size())), qint64(text.size()));
        }
    }
};
QTEST_GUILESS_MAIN(DxbcPixelPrimitiveMaskTests)
#include "DxbcPixelPrimitiveMaskTests.moc"
