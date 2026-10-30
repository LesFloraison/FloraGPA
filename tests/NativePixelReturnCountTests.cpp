#include "PixelOrdinalCapture.h"
#include "PredicateCapture.h"
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
ReplayOptions options(uint32_t vendor) {
    ReplayOptions result;
    result.debug = true;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    result.until = 100;
    result.before = result.prepareBeforeDraw = true;
    return result;
}
void save(const Json &value) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_RETURN_COUNT_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create return count evidence directory");
    QFile file(root + "/" + QString::fromLatin1(QTest::currentTestFunction()) + "-" +
               QString::fromLatin1(QTest::currentDataTag()) + ".json");
    const auto text = value.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save return count evidence");
}
void retainFrame(const QString &path) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_RETURN_COUNT_EVIDENCE");
    if (root.isEmpty())
        return;
    QDir().mkpath(root);
    const auto destination = root + "/" + QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                             QString::fromLatin1(QTest::currentDataTag()) + ".gpa_frame";
    if (!QFile::copy(path, destination))
        throw std::runtime_error("Cannot retain return count fixture");
}
std::vector<uint8_t> storage(Replay &r, Id resource, uint32_t samples, uint32_t sample) {
    return samples == 1 ? r.readTexture(resource) : r.readMsaa(resource, sample, 2).bytes;
}
std::vector<uintptr_t> boundState(Replay &replay) {
    std::vector<uintptr_t> result;
    replay.inspectNativeState([&](auto *context, const auto &) {
        const auto retain = [&](IUnknown *object) {
            result.push_back(reinterpret_cast<uintptr_t>(object));
            if (object)
                object->Release();
        };
        ID3D11RenderTargetView *targets[8]{};
        ID3D11DepthStencilView *depth = nullptr;
        context->OMGetRenderTargets(8, targets, &depth);
        for (auto *target : targets)
            retain(target);
        retain(depth);
        ID3D11DepthStencilState *state = nullptr;
        UINT reference = 0;
        context->OMGetDepthStencilState(&state, &reference);
        retain(state);
        result.push_back(reference);
        ID3D11UnorderedAccessView *uavs[8]{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
        for (auto *uav : uavs)
            retain(uav);
        ID3D11Predicate *predicate = nullptr;
        BOOL value = FALSE;
        context->GetPredication(&predicate, &value);
        retain(predicate);
        result.push_back(value);
        ID3D11PixelShader *shader = nullptr;
        ID3D11ClassInstance *classes[256]{};
        UINT count = 256;
        context->PSGetShader(&shader, classes, &count);
        retain(shader);
        result.push_back(count);
        for (UINT i = 0; i < count; ++i)
            retain(classes[i]);
    });
    return result;
}
} // namespace
class NativePixelReturnCountTests final : public QObject {
    Q_OBJECT
  private slots:
    void decode() {
        for (uint32_t count : {0u, 1u, 260u, 65537u, 16777215u}) {
            const auto bits = std::bit_cast<uint32_t>(float(count));
            const auto result = decodeNativePixelReturnCount({bits, bits, bits, 0x7fc00000});
            QCOMPARE(result.at("valid"), Json(true));
            QCOMPARE(result.at("count"), Json(count));
        }
        for (uint32_t bits :
             {0x4b800000u, 0x4b800001u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xbf800000u, 0x3f000000u}) {
            const auto result = decodeNativePixelReturnCount({bits, bits, bits, 0});
            QCOMPARE(result.at("valid"), Json(false));
            QVERIFY(result.at("count").is_null());
        }
        QVERIFY(decodeNativePixelReturnCount({0, 0x3f800000, 0, 0}).at("count").is_null());
    }
    void count_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("layout");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<uint32_t>("targets");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t layout : {0u, 1u, 2u})
                for (uint32_t samples : {1u, 4u})
                    for (uint32_t mode : {0u, 1u, 2u, 3u})
                        for (uint32_t targets : {1u, 8u})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(layout)
                                                         .arg(samples)
                                                         .arg(mode)
                                                         .arg(targets)))
                                << vendor << layout << samples << mode << targets;
    }
    void count() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, layout);
            QFETCH(uint32_t, samples);
            QFETCH(uint32_t, mode);
            QFETCH(uint32_t, targets);
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            pixelOrdinalCapture(layout, samples, mode, targets).save(path);
            retainFrame(path);
            Frame frame(path.toStdWString());
            auto configuration = options(vendor);
            Replay replay(frame, configuration);
            replay.run();
            std::vector<std::vector<uint8_t>> initial;
            for (uint32_t target = 0; target < targets; ++target)
                for (uint32_t sample = 0; sample < samples; ++sample)
                    initial.push_back(storage(replay, 1000 + 4 * target, samples, sample));
            auto ordinaryConfig = configuration;
            ordinaryConfig.before = ordinaryConfig.prepareBeforeDraw = false;
            Replay ordinary(frame, ordinaryConfig);
            ordinary.run();
            std::vector<std::vector<uint8_t>> referenceOther;
            for (uint32_t target = 0; target + 1 < targets; ++target)
                for (uint32_t sample = 0; sample < samples; ++sample)
                    referenceOther.push_back(storage(ordinary, 1000 + 4 * target, samples, sample));
            const auto bindings = boundState(replay);
            Json records = Json::array();
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const auto report = replayNativePixelReturnCount(
                    replay, 100, {targets - 1, 0, 0, 0, sample}, [&](Replay &privateReplay, bool after) {
                        if (!after)
                            return;
                        size_t index = 0;
                        for (uint32_t target = 0; target + 1 < targets; ++target)
                            for (uint32_t selected = 0; selected < samples; ++selected)
                                QCOMPARE(storage(privateReplay, 1000 + 4 * target, samples, selected),
                                         referenceOther[index++]);
                    });
                QCOMPARE(boundState(replay), bindings);
                records.push_back(report);
            }
            save({{"vendor", vendor},
                  {"layout", layout},
                  {"samples", samples},
                  {"mode", mode},
                  {"targets", targets},
                  {"reports", records}});
            for (const auto &report : records) {
                QCOMPARE(report.at("return_count").at("count"), Json(mode ? 130 : 260));
                QCOMPARE(report.at("return_count").at("valid"), Json(true));
                QVERIFY(report.at("original_fragment_eligibility").is_null());
                QCOMPARE(report.at("extra_uav_slots"), Json(0));
                QCOMPARE(report.at("backend"), Json("native-pixel-return-count/1"));
            }
            size_t i = 0;
            for (uint32_t target = 0; target < targets; ++target)
                for (uint32_t sample = 0; sample < samples; ++sample)
                    QCOMPARE(storage(replay, 1000 + 4 * target, samples, sample), initial[i++]);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void transport_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (auto mode : {"unorm", "srgb", "float16", "mip-array", "msaa-array", "alpha-zero",
                              "sample-mask", "disabled", "depth", "stencil", "write-mask"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void transport() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, mode);
            const bool array = mode == "mip-array" || mode == "msaa-array";
            const uint32_t samples =
                mode == "msaa-array" || mode == "alpha-zero" || mode == "sample-mask" ? 4 : 1;
            const uint32_t format = mode == "unorm" ? 28 : mode == "srgb" ? 29 : mode == "float16" ? 10 : 2;
            const uint32_t mip = mode == "mip-array" ? 1 : 0;
            const bool depth = mode == "depth", stencil = mode == "stencil";
            auto capture = pixelOrdinalCapture(0, samples, 0, 1);
            std::erase_if(capture.entries, [](const auto &e) {
                return e.id == 1000 || e.id == 1001 || e.id == 1002 || e.id == 70 || e.id == 90 ||
                       e.id == 190;
            });
            capture.add(1000, 5, 0x85,
                        statePack(Id(0), Id(0), 1u << mip, 1u << mip, mip + 1, array ? 4u : 1u, format,
                                  samples, 0u, 0u, 32u, 0u, 0u, Id(0)));
            capture.add(1002, 5, 0x8d,
                        statePack(Id(0), Id(0), Id(1000), format,
                                  samples > 1 ? (array ? 7u : 6u) : (array ? 5u : 4u),
                                  samples > 1 && array ? 1u : mip, array ? (samples > 1 ? 2u : 1u) : 0u,
                                  array && samples == 1 ? 2u : 0u));
            capture.add(70, 7, 0x32, statePack(Id(0), Id(1), Id(1002), uint8_t(1), .25f, .5f, .75f, 1.f));
            addOrdinalCode(
                capture, 11,
                "float4 main(uint v:SV_VertexID):SV_Position{uint k=v%3;return float4(k==2?3:-1,k==1?3:-1," +
                    std::string(depth ? "(v/3)%2?.75:.25" : "0") + ",1);}",
                "vs_5_0");
            addOrdinalCode(capture, 13,
                           "float4 main():SV_Target{return float4(.25,.5,.75," +
                               std::string(mode == "alpha-zero" ? "0" : "1") + ");}",
                           "ps_5_0");
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
            if (array) {
                capture.add(60, 5, 0x91, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), Id(61)));
                addOrdinalCode(capture, 61,
                               "struct V{float4 p:SV_Position;};struct G{float4 p:SV_Position;uint "
                               "l:SV_RenderTargetArrayIndex;};"
                               "[maxvertexcount(3)]void main(triangle V v[3],uint id:SV_PrimitiveID,inout "
                               "TriangleStream<G> s){"
                               "for(uint i=0;i<3;i++){G o;o.p=v[i].p;o.l=id%2;s.Append(o);}}",
                               "gs_5_0");
                state.stages[3].shader = 60;
            }
            if (depth || stencil) {
                capture.add(720, 5, 0x85,
                            statePack(Id(0), Id(0), 1u, 1u, 1u, 1u, 45u, 1u, 0u, 0u, 64u, 0u, 0u, Id(0)));
                capture.add(722, 5, 0x8e, statePack(Id(0), Id(0), Id(720), 45u, 3u, 0u, 0u, 0u, 0u));
                capture.add(80, 7, 0x31, statePack(Id(0), Id(1), Id(722), 3u, .5f, uint8_t(0)));
                state.dsv = 722;
            }
            capture.add(90, 3, 3, snapshot(state));
            capture.add(190, 3, 3, snapshot(state));
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            capture.save(path);
            retainFrame(path);
            Frame frame(path.toStdWString());
            auto config = options(vendor);
            if (mode == "disabled")
                config.disabled.insert(100);
            if (mode == "sample-mask")
                config.blendEdits[100].sampleMask = 2;
            if (mode == "alpha-zero" || mode == "write-mask") {
                auto blend = defaultBlend();
                blend.AlphaToCoverageEnable = mode == "alpha-zero";
                if (mode == "write-mask")
                    blend.RenderTarget[0].RenderTargetWriteMask = 0;
                config.blendEdits[100].descriptor = blend;
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
                config.depthStencilEdits[100].descriptor = ds;
                config.depthStencilEdits[100].reference = 0;
            }
            Replay replay(frame, config);
            replay.run();
            const auto before = samples == 1 ? replay.readTexture(1000) : std::vector<uint8_t>();
            const auto beforeDepth = depth || stencil ? replay.readTexture(720) : std::vector<uint8_t>();
            auto ordinaryConfig = config;
            ordinaryConfig.before = ordinaryConfig.prepareBeforeDraw = false;
            Replay ordinary(frame, ordinaryConfig);
            ordinary.run();
            const auto expectedDepth = depth || stencil ? ordinary.readTexture(720) : std::vector<uint8_t>();
            Json reports = Json::array();
            for (uint32_t layer = 0; layer < (array ? 2u : 1u); ++layer)
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const auto report = replayNativePixelReturnCount(
                        replay, 100, {0, 0, 0, layer, sample}, [&](Replay &privateReplay, bool after) {
                            if (after && (depth || stencil))
                                QCOMPARE(privateReplay.readTexture(720), expectedDepth);
                        });
                    const uint32_t expected =
                        mode == "alpha-zero" || mode == "disabled" || (mode == "sample-mask" && sample != 1)
                            ? 0
                        : array            ? 130
                        : depth || stencil ? 1
                                           : 260;
                    reports.push_back({{"expected", expected}, {"report", report}});
                }
            save(
                {{"vendor", vendor}, {"mode", mode.toStdString()}, {"format", format}, {"reports", reports}});
            for (const auto &row : reports)
                QCOMPARE(row.at("report").at("return_count").at("count"), row.at("expected"));
            if (samples == 1)
                QCOMPARE(replay.readTexture(1000), before);
            if (depth || stencil)
                QCOMPARE(replay.readTexture(720), beforeDepth);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void counters_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("counter");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool counter : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(counter))) << vendor << counter;
    }
    void counters() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(bool, counter);
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            graphicsCounterCapture(counter).save(path);
            retainFrame(path);
            Frame frame(path.toStdWString());
            auto config = options(vendor);
            config.until = 200;
            Replay replay(frame, config);
            replay.run();
            const auto bytes = replay.readBuffer(10);
            const auto initial = replay.readCounter(12);
            auto ordinaryConfig = config;
            ordinaryConfig.before = ordinaryConfig.prepareBeforeDraw = false;
            Replay ordinary(frame, ordinaryConfig);
            ordinary.run();
            const auto expectedBytes = ordinary.readBuffer(10);
            const auto expectedCounter = ordinary.readCounter(12);
            const auto report = replayNativePixelReturnCount(replay, 200, {}, [&](Replay &r, bool after) {
                if (after) {
                    QCOMPARE(r.readBuffer(10), expectedBytes);
                    QCOMPARE(r.readCounter(12), expectedCounter);
                }
            });
            QCOMPARE(report.at("return_count").at("count"), Json(1));
            QCOMPARE(replay.readBuffer(10), bytes);
            QCOMPARE(replay.readCounter(12), initial);
            save({{"vendor", vendor},
                  {"counter", counter},
                  {"initial_counter", initial},
                  {"private_counter", expectedCounter},
                  {"restored_counter", replay.readCounter(12)},
                  {"report", report}});
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
        try {
            QFETCH(uint32_t, vendor);
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            pixelOrdinalCapture(0, 1, 0, 1).save(path);
            retainFrame(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, options(vendor));
            replay.run();
            const auto before = replay.readTexture(1000);
            for (bool after : {false, true}) {
                QVERIFY_THROWS_EXCEPTION(
                    std::runtime_error,
                    replayNativePixelReturnCount(replay, 100, {}, [after](Replay &, bool completed) {
                        if (after == completed)
                            throw std::runtime_error("Injected count observer failure");
                    }));
                QCOMPARE(replay.readTexture(1000), before);
            }
            bool cancel = false;
            QVERIFY_THROWS_EXCEPTION(std::exception,
                                     replayNativePixelReturnCount(
                                         replay, 100, {},
                                         [&](Replay &, bool after) {
                                             if (!after)
                                                 cancel = true;
                                         },
                                         256ull * 1024 * 1024, 30000, [&] { return cancel; }));
            QCOMPARE(replay.readTexture(1000), before);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     replayNativePixelReturnCount(replay, 100, {}, {}, 1));
            QCOMPARE(replay.readTexture(1000), before);
            for (auto invalid :
                 {NativePixelReturnCountSelection{8}, {0, 1}, {0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0, 1}})
                QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                                         replayNativePixelReturnCount(replay, 100, invalid));
            cancel = false;
            QVERIFY_THROWS_EXCEPTION(std::exception,
                                     replayNativePixelReturnCount(
                                         replay, 100, {},
                                         [&](Replay &, bool after) {
                                             if (after)
                                                 cancel = true;
                                         },
                                         256ull * 1024 * 1024, 30000, [&] { return cancel; }));
            QCOMPARE(replay.readTexture(1000), before);
            QVERIFY_THROWS_EXCEPTION(std::exception, replayNativePixelReturnCount(
                                                         replay, 100, {},
                                                         [](Replay &, bool after) {
                                                             if (!after)
                                                                 QThread::msleep(4);
                                                         },
                                                         256ull * 1024 * 1024, 1));
            QCOMPARE(replay.readTexture(1000), before);
            const auto integerPath = directory.filePath("integer.gpa_frame");
            blendCapture(1, false, true).save(integerPath);
            Frame integerFrame(integerPath.toStdWString());
            Replay integer(integerFrame, options(vendor));
            integer.run();
            const auto integerBefore = integer.readTexture(1000);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayNativePixelReturnCount(integer, 100, {}));
            QCOMPARE(integer.readTexture(1000), integerBefore);
            Json predicates = Json::array();
            for (bool skip : {false, true}) {
                const auto predicatePath = directory.filePath(skip ? "skip.gpa_frame" : "pass.gpa_frame");
                predicateCapture(true, skip ? 1 : 0).save(predicatePath);
                Frame predicateFrame(predicatePath.toStdWString());
                auto predicateOptions = options(vendor);
                predicateOptions.until = 2000;
                Replay predicated(predicateFrame, predicateOptions);
                predicated.run();
                const auto initialPredicate = boundState(predicated);
                const auto predicateReport = replayNativePixelReturnCount(predicated, 2000, {});
                QCOMPARE(predicateReport.at("return_count").at("count"), Json(skip ? 0 : 1));
                QCOMPARE(boundState(predicated), initialPredicate);
                predicates.push_back({{"skip", skip}, {"report", predicateReport}});
            }
            const auto retry = replayNativePixelReturnCount(replay, 100, {});
            QCOMPARE(retry.at("return_count").at("count"), Json(260));
            QCOMPARE(replay.readTexture(1000), before);
            save({{"vendor", vendor},
                  {"observer_failures_restored", 2},
                  {"cancellation_restored", true},
                  {"cancellation_points", 2},
                  {"timeout_restored", true},
                  {"integer_output_rejected", true},
                  {"budget_restored", true},
                  {"invalid_selections_rejected", 5},
                  {"predicates", predicates},
                  {"retry", retry}});
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativePixelReturnCountTests)
#include "NativePixelReturnCountTests.moc"
