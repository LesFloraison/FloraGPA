#include "BlendCapture.h"
#include "application/NativePixelDrawHistory.h"
#include "application/NativePixelIdentity.h"
#include "replay/BlendState.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
void save(const Json &value) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_PRESENCE_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create pixel presence evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto bytes = value.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()))
        throw std::runtime_error("Cannot write pixel presence evidence");
}
NativePixelPredicateStorage pixel(Replay &r, Id resource, uint32_t samples, uint32_t sample,
                                  uint32_t format) {
    return {format, samples > 1 ? r.readMsaa(resource, sample, format).bytes : r.readTexture(resource)};
}
} // namespace
class NativePixelPresenceTests final : public QObject {
    Q_OBJECT
  private slots:
    void recovery_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor) + "-recovery")) << vendor;
    }
    void recovery() {
        try {
            QFETCH(uint32_t, vendor);
            QTemporaryDir dir;
            const auto path = dir.filePath("frame.gpa_frame");
            blendCapture().save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.debug = true;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            options.until = 100;
            options.before = options.prepareBeforeDraw = true;
            Replay replay(frame, options);
            replay.run();
            const auto before = replay.readTexture(1000);
            for (bool completed : {false, true}) {
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         replayNativePixelPrimitivePresence(
                                             replay, 100, 0, {UINT32_MAX, 0}, [&](Replay &, bool after) {
                                                 if (after == completed)
                                                     throw std::runtime_error(
                                                         "Injected presence observer failure");
                                             }));
                QCOMPARE(replay.readTexture(1000), before);
            }
            bool cancel = false;
            QVERIFY_THROWS_EXCEPTION(std::exception,
                                     replayNativePixelPrimitivePresence(
                                         replay, 100, 0, {UINT32_MAX, 0},
                                         [&](Replay &, bool after) {
                                             if (!after)
                                                 cancel = true;
                                         },
                                         256ull * 1024 * 1024, 30000, [&] { return cancel; }));
            QCOMPARE(replay.readTexture(1000), before);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayNativePixelPrimitivePresence(
                                                             replay, 100, 0, {UINT32_MAX, 0}, {}, 1));
            QCOMPARE(replay.readTexture(1000), before);
            Json result;
            replayNativePixelPrimitivePresence(replay, 100, 0, {UINT32_MAX, 0}, [&](Replay &r, bool after) {
                if (after)
                    result = decodeNativePixelPredicate(pixel(r, 1000, 1, 0, 2));
            });
            QCOMPARE(result.at("rgb_predicate"), Json(true));
            QCOMPARE(replay.readTexture(1000), before);
            const auto integerPath = dir.filePath("integer.gpa_frame");
            blendCapture(1, false, true).save(integerPath);
            Frame integerFrame(integerPath.toStdWString());
            Replay integer(integerFrame, options);
            integer.run();
            const auto integerBefore = integer.readTexture(1000);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     replayNativePixelPrimitivePresence(integer, 100, 0, {UINT32_MAX, 0}));
            QCOMPARE(integer.readTexture(1000), integerBefore);
            save({{"mode", "recovery"},
                  {"vendor", vendor},
                  {"observer_failures_restored", 2},
                  {"cancelled_restored", true},
                  {"budget_restored", true},
                  {"unsupported_format_restored", true},
                  {"retry", result}});
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void draw_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"overlap", "discard-first", "discard-middle", "discard-last",
                                     "early-discard", "sample", "sample-mask", "alpha-zero", "write-mask",
                                     "mrt-eight", "unorm", "srgb", "depth", "stencil", "duplicate-gs"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void draw() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            const uint32_t samples =
                mode == "sample" || mode == "sample-mask" || mode == "alpha-zero" ? 4 : 1;
            const uint32_t format = mode == "unorm" ? 28 : mode == "srgb" ? 29 : 2;
            const uint32_t targets = mode == "mrt-eight" ? 8 : 1, selected = targets - 1;
            const Id resource = 1000 + 4 * selected;
            auto c = blendCapture(targets, false, false, samples);
            for (const auto &entry : c.entries)
                if (entry.id == 100)
                    put(c.bytes, size_t(entry.offset) + 24, 9u);
            if (format != 2) {
                std::erase_if(c.entries,
                              [](const auto &entry) { return entry.id >= 1000 && entry.id <= 1002; });
                c.add(1000, 5, 0x85,
                      statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, format, 1u, 0u, 0u, 32u, 0u, 0u, Id(1001)));
                c.add(1001, 9, 1, statePack(4u, 0x40208020u));
                c.add(1002, 5, 0x8d, statePack(Id(0), Id(0), Id(1000), format, 4u, 0u, 0u, 0u));
            }
            const bool depth = mode == "depth", stencil = mode == "stencil";
            if (depth || stencil) {
                c.add(720, 5, 0x85,
                      statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, 45u, 1u, 0u, 0u, 64u, 0u, 0u, Id(0)));
                c.add(722, 5, 0x8e, statePack(Id(0), Id(0), Id(720), 45u, 3u, 0u, 0u, 0u, 0u));
                c.add(80, 7, 0x31, statePack(Id(0), Id(1), Id(722), 3u, .5f, uint8_t(0)));
                std::erase_if(c.entries, [](const auto &entry) { return entry.id == 90 || entry.id == 190; });
                State state{};
                state.topology = 4;
                state.sampleMask = UINT32_MAX;
                state.omStart = 8;
                state.rtCount = 1;
                state.blend = 64;
                state.rasterizer = 52;
                state.viewports = 50;
                state.stages[0].shader = 10;
                state.stages[4].shader = 12;
                state.rtv[0] = 1002;
                state.dsv = 722;
                c.add(90, 3, 3, snapshot(state));
                c.add(190, 3, 3, snapshot(state));
            }
            if (mode == "duplicate-gs") {
                const auto code = compileClassProgram(
                    "struct V{float4 p:SV_Position;};struct O{float4 p:SV_Position;uint id:SV_PrimitiveID;};"
                    "[maxvertexcount(3)]void main(triangle V v[3],inout TriangleStream<O> s){"
                    "[unroll]for(uint i=0;i<3;i++){O o;o.p=v[i].p;o.id=7;s.Append(o);}}",
                    "gs_5_0");
                c.add(60, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(61)));
                auto raw = statePack(uint64_t(code.size()));
                raw.insert(raw.end(), code.begin(), code.end());
                append(raw, Id(0));
                c.add(61, 9, 0x81, raw);
                std::erase_if(c.entries, [](const auto &entry) { return entry.id == 90 || entry.id == 190; });
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
            std::string source = "struct O{";
            for (uint32_t target = 0; target < targets; ++target)
                source += "float4 t" + std::to_string(target) + ":SV_Target" + std::to_string(target) + ";";
            source += "};";
            if (mode == "early-discard")
                source += "[earlydepthstencil]";
            source += mode == "duplicate-gs" ? "O main(float4 p:SV_Position,uint id:SV_PrimitiveID"
                                             : "O main(uint id:SV_PrimitiveID";
            if (samples > 1)
                source += ",uint sample:SV_SampleIndex";
            source += "){";
            if (mode == "discard-first")
                source += "if(id==0)discard;";
            if (mode == "discard-middle" || mode == "early-discard")
                source += "if(id==1)discard;";
            if (mode == "discard-last")
                source += "if(id==2)discard;";
            if (mode == "sample")
                source += "if(id==1 && sample==2)discard;";
            source += "O o;";
            for (uint32_t target = 0; target < targets; ++target)
                source += "o.t" + std::to_string(target) + "=float4(id*.25," +
                          (mode == "duplicate-gs" ? std::string("p.x") : std::string(".5")) + ",.75," +
                          (mode == "alpha-zero" ? std::string("0") : std::string(".5")) + ");";
            source += "return o;}";
            QTemporaryDir dir;
            const auto path = dir.filePath("frame.gpa_frame");
            c.save(path);
            Frame frame(path.toStdWString());
            ReplayOptions options;
            options.debug = true;
            options.warp = !vendor;
            if (vendor)
                options.vendor = vendor;
            options.until = 100;
            options.shaders[10] = compileClassProgram(
                "float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return float4(i==2?3:-1,i==1?3:-1," +
                    std::string(depth ? "v/3==1?.75:.25" : "0") + ",1);}",
                "vs_5_0");
            options.shaders[12] = compileClassProgram(source, "ps_5_0");
            if (mode == "sample-mask")
                options.blendEdits[100].sampleMask = 2u;
            if (mode == "write-mask" || mode == "alpha-zero") {
                auto blend = defaultBlend();
                blend.AlphaToCoverageEnable = mode == "alpha-zero";
                if (mode == "write-mask")
                    blend.RenderTarget[0].RenderTargetWriteMask = 0;
                options.blendEdits[100].descriptor = blend;
            }
            if (depth || stencil) {
                D3D11_DEPTH_STENCIL_DESC ds{};
                ds.DepthEnable = depth;
                ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
                ds.DepthFunc = D3D11_COMPARISON_LESS;
                ds.StencilEnable = stencil;
                ds.StencilReadMask = ds.StencilWriteMask = 255;
                ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                              D3D11_STENCIL_OP_INCR_SAT, D3D11_COMPARISON_EQUAL};
                options.depthStencilEdits[100].descriptor = ds;
                options.depthStencilEdits[100].reference = 0;
            }
            Replay reference(frame, options);
            reference.run();
            std::vector<std::vector<uint8_t>> referenceOther;
            for (uint32_t target = 0; target < selected; ++target)
                referenceOther.push_back(reference.readTexture(1000 + target * 4));
            const auto referenceDepth =
                depth || stencil ? reference.readTexture(720) : std::vector<uint8_t>();
            options.before = options.prepareBeforeDraw = true;
            Replay replay(frame, options);
            replay.run();
            std::vector<std::vector<uint8_t>> original;
            for (uint32_t sample = 0; sample < samples; ++sample)
                original.push_back(pixel(replay, resource, samples, sample, format).bytes);
            const auto originalDepth = depth || stencil ? replay.readTexture(720) : std::vector<uint8_t>();
            Json passes = Json::array();
            for (uint32_t identity = 0; identity < 4; ++identity) {
                const uint32_t systemId = mode == "duplicate-gs" && identity == 0 ? 7 : identity;
                Json before = Json::array(), after = Json::array(), others = Json::array();
                std::vector<uint8_t> observedDepth;
                auto report = replayNativePixelPrimitivePresence(
                    replay, 100, selected, {UINT32_MAX, systemId},
                    [&](Replay &privateReplay, bool completed) {
                        for (uint32_t sample = 0; sample < samples; ++sample)
                            (completed ? after : before)
                                .push_back(decodeNativePixelPredicate(
                                    pixel(privateReplay, resource, samples, sample, format)));
                        if (completed) {
                            for (uint32_t target = 0; target < selected; ++target) {
                                auto bytes = privateReplay.readTexture(1000 + target * 4);
                                others.push_back(bytes);
                                if (bytes != referenceOther[target])
                                    throw std::runtime_error("Presence changed another MRT");
                            }
                            if (depth || stencil)
                                observedDepth = privateReplay.readTexture(720);
                            if (observedDepth != referenceDepth)
                                throw std::runtime_error("Presence changed depth/stencil evolution");
                        }
                    });
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const bool expected =
                        (mode == "duplicate-gs" ? systemId == 7 : identity < 3) &&
                        !(mode == "discard-first" && identity == 0) &&
                        !((mode == "discard-middle" || mode == "early-discard") && identity == 1) &&
                        !(mode == "discard-last" && identity == 2) &&
                        !(mode == "sample" && identity == 1 && sample == 2) &&
                        !(mode == "sample-mask" && sample != 1) && mode != "alpha-zero" &&
                        !((depth || stencil) && identity != 0);
                    QCOMPARE(before[sample].at("rgb_predicate"), Json(false));
                    QCOMPARE(after[sample].at("rgb_predicate"), Json(expected));
                    QCOMPARE(pixel(replay, resource, samples, sample, format).bytes, original[sample]);
                }
                if (depth || stencil)
                    QCOMPARE(replay.readTexture(720), originalDepth);
                QCOMPARE(report.at("completion_target").at("rgb_reduction"), Json("maximum"));
                QVERIFY(report.at("original_fragment_eligibility").is_null());
                passes.push_back({{"identity", systemId},
                                  {"before", before},
                                  {"after", after},
                                  {"observation", report},
                                  {"other_targets", others},
                                  {"depth_stencil", observedDepth}});
            }
            // A later zero really does erase the old nonaccumulating observation.
            if (mode == "overlap") {
                Json last;
                replayNativePixelPrimitiveMask(replay, 100, 0, {UINT32_MAX, 0}, [&](Replay &r, bool after) {
                    if (after)
                        last = decodeNativePixelPredicate(pixel(r, resource, samples, 0, format));
                });
                QCOMPARE(last.at("rgb_predicate"), Json(false));
            }
            Json history;
            if (mode == "overlap" || mode == "discard-middle" || mode == "duplicate-gs") {
                NativePixelDrawHistoryLimits limits;
                limits.observePrimitivePresence = true;
                NativeHistorySelection selection;
                selection.resource = resource;
                history = collectNativePixelDrawHistory(replay, 100, selection, limits);
                const auto &presence = history.at("primitive_presence_observation");
                QCOMPARE(presence.at("status"), Json("observed"));
                QCOMPARE(presence.at("observations").size(), size_t(mode == "duplicate-gs" ? 1 : 3));
                QCOMPARE(presence.at("pass_count"), Json(mode == "duplicate-gs" ? 2 : 6));
                if (mode == "duplicate-gs")
                    QCOMPARE(presence.at("observations")[0].at("matching_primitive_ordinals"),
                             Json({0, 1, 2}));
                for (const auto &row : history.at("records")) {
                    const auto &observed = presence.at("observations")
                                               .at(row.at("system_id_presence_observation").get<size_t>());
                    QCOMPARE(observed.at("system_primitive_id"), row.at("original_primitive_id"));
                    QCOMPARE(observed.at("color_completion_observed"),
                             Json(mode != "discard-middle" || row.at("original_primitive_id") != 1));
                    QVERIFY(row.at("original_fragment_eligibility").is_null());
                }
            }
            Json restored = Json::array();
            for (uint32_t sample = 0; sample < samples; ++sample)
                restored.push_back(pixel(replay, resource, samples, sample, format).bytes);
            save({{"mode", mode.toStdString()},
                  {"vendor", vendor},
                  {"passes", passes},
                  {"history", history},
                  {"initial_storage", original},
                  {"restored_storage", restored},
                  {"reference_other_targets", referenceOther},
                  {"reference_depth_stencil", referenceDepth},
                  {"initial_depth_stencil", originalDepth},
                  {"restored_depth_stencil",
                   depth || stencil ? replay.readTexture(720) : std::vector<uint8_t>()},
                  {"original_storage_restored", true}});
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativePixelPresenceTests)
#include "NativePixelPresenceTests.moc"
