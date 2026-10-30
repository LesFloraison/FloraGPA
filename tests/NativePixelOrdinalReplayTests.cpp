#include "DepthStencilCapture.h"
#include "PixelOrdinalCapture.h"
#include "PredicateCapture.h"
#include "application/NativePixelGeometryReplay.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
ReplayOptions options(uint32_t vendor, Id event) {
    ReplayOptions result;
    result.debug = true;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    result.until = event;
    result.before = result.prepareBeforeDraw = true;
    return result;
}
void save(const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_ORDINAL_REPLAY_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create ordinal replay evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                                   QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto text = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save ordinal replay evidence");
}
std::array<uint32_t, 4> pixel(Replay &replay, Id resource, uint32_t samples, uint32_t sample) {
    const auto data =
        samples == 1 ? replay.readTexture(resource) : replay.readMsaa(resource, sample, 2).bytes;
    if (data.size() != 16)
        throw std::runtime_error("Unexpected ordinal test texture size");
    std::array<uint32_t, 4> result{};
    std::memcpy(result.data(), data.data(), 16);
    return result;
}
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
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
class NativePixelOrdinalReplayTests final : public QObject {
    Q_OBJECT
  private slots:
    void extraction_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("layout");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<uint32_t>("targets");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t layout = 0; layout < 3; ++layout)
                for (uint32_t samples : {1u, 4u})
                    for (uint32_t mode = 0; mode < 4; ++mode)
                        for (uint32_t targets : {1u, 8u})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(layout)
                                                         .arg(samples)
                                                         .arg(mode)
                                                         .arg(targets)))
                                << vendor << layout << samples << mode << targets;
    }
    void extraction() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, layout);
        QFETCH(uint32_t, samples);
        QFETCH(uint32_t, mode);
        QFETCH(uint32_t, targets);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        pixelOrdinalCapture(layout, samples, mode, targets).save(path);
        Frame frame(path.toStdWString());
        Replay replay(frame, options(vendor, 100));
        replay.run();
        const auto resource = 1000 + 4 * (targets - 1);
        std::vector<std::array<uint32_t, 4>> before;
        for (uint32_t sample = 0; sample < samples; ++sample)
            before.push_back(pixel(replay, resource, samples, sample));
        const auto bindings = boundState(replay);
        if (mode == 3 && samples == 1) {
            for (bool after : {false, true}) {
                QVERIFY_THROWS_EXCEPTION(
                    std::runtime_error,
                    replayNativePixelOrdinal(replay, 100, {targets - 1, 0}, [&](Replay &, bool observed) {
                        if (observed == after)
                            throw std::runtime_error("Injected normalized coverage failure");
                    }));
                QCOMPARE(boundState(replay), bindings);
                QCOMPARE(pixel(replay, resource, samples, 0), before[0]);
            }
            bool cancelled = false;
            QVERIFY_THROWS_EXCEPTION(
                std::exception, replayNativePixelOrdinal(
                                    replay, 100, {targets - 1, 0}, [&](Replay &, bool) { cancelled = true; },
                                    256ull * 1024 * 1024, 30000, [&] { return cancelled; }));
            QCOMPARE(boundState(replay), bindings);
            QCOMPARE(pixel(replay, resource, samples, 0), before[0]);
        }
        Json evidence{{"vendor", vendor},   {"layout", layout}, {"samples", samples},   {"mode", mode},
                      {"targets", targets}, {"before", before}, {"rows", Json::array()}};
        for (uint32_t rank : {0u, 1u, 2u, 127u, 253u, 254u}) {
            for (bool marked : {false, true}) {
                std::vector<std::array<uint32_t, 4>> actual;
                const auto report = replayNativePixelOrdinal(
                    replay, 100, {targets - 1, rank, marked}, [&](Replay &r, bool after) {
                        if (after)
                            for (uint32_t sample = 0; sample < samples; ++sample)
                                actual.push_back(pixel(r, resource, samples, sample));
                    });
                const auto &selection = report.at("ordinal_selection");
                QCOMPARE(boundState(replay), bindings);
                QCOMPARE(selection.at("force_early_depth_stencil"), Json(mode == 2));
                QCOMPARE(selection.at("stencil_payload_bytes"), Json(4 * samples));
                QCOMPARE(selection.at("application_uav_bindings_retained"), Json(true));
                QVERIFY(report.at("original_fragment_eligibility").is_null());
                QCOMPARE(report.at("extra_uav_slots"), Json(0));
                QCOMPARE(!report.at("coverage_normalization").is_null(), mode == 3 && samples == 1);
                std::vector<std::array<uint32_t, 4>> expected;
                for (uint32_t sample = 0; sample < samples; ++sample) {
                    const auto fragment = mode == 1 ? rank * 2 : mode == 3 ? rank * 2 + sample % 2 : rank;
                    const bool present = fragment < 260 && !(mode == 2 && (fragment & 1));
                    auto value =
                        present ? std::array{bits(float(fragment + 1)),
                                             bits(float(layout == 2   ? 7
                                                        : layout == 1 ? 0
                                                                      : fragment)),
                                             bits(float(layout == 1 ? fragment : 0)), bits(float(sample))}
                                : before[sample];
                    if (marked)
                        value = present ? std::array{bits(1), bits(2), bits(3), bits(float(sample))}
                                        : std::array<uint32_t, 4>{};
                    expected.push_back(value);
                    QCOMPARE(pixel(replay, resource, samples, sample), before[sample]);
                }
                evidence["rows"].push_back({{"report", report}, {"actual", actual}, {"expected", expected}});
            }
        }
        save(evidence);
        for (const auto &row : evidence["rows"])
            QVERIFY2(row["actual"] == row["expected"], row.dump().c_str());
    }
    void bindings_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode :
                 {"append", "counter", "classes", "predicate_pass", "predicate_skip", "depth"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void bindings() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        const bool predicated = mode.startsWith("predicate");
        const bool counter = mode == "append" || mode == "counter";
        auto capture = mode == "depth"     ? depthStencilCapture()
                       : predicated        ? predicateCapture(true, mode == "predicate_skip" ? 1 : 0)
                       : mode == "classes" ? graphicsClassCapture()
                                           : graphicsCounterCapture(mode == "counter");
        const Id event = mode == "depth" ? 1000 : predicated ? 2000 : 200;
        if (mode == "depth") {
            addOrdinalCode(
                capture, 31,
                "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}",
                "vs_5_0");
            addOrdinalCode(capture, 33, "float4 main():SV_Target{return float4(1,0,0,1);}", "ps_5_0");
        }
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        auto opts = options(vendor, event);
        opts.before = opts.prepareBeforeDraw = false;
        Replay reference(frame, opts);
        reference.run();
        const auto expected = reference.readTexture(20);
        const auto expectedBuffer = counter ? reference.readBuffer(10) : std::vector<uint8_t>{};
        const auto expectedCounter = counter ? reference.readCounter(12) : 0;
        opts.before = opts.prepareBeforeDraw = true;
        Replay replay(frame, opts);
        replay.run();
        const auto before = replay.readTexture(20);
        const auto beforeBuffer = counter ? replay.readBuffer(10) : std::vector<uint8_t>{};
        const auto beforeCounter = counter ? replay.readCounter(12) : 0;
        const auto beforeDepth = mode == "depth" ? replay.readTexture(720) : std::vector<uint8_t>{};
        const auto bindingsBefore = boundState(replay);
        for (bool after : {false, true}) {
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                replayNativePixelOrdinal(replay, event, {}, [&](Replay &, bool observedAfter) {
                    if (after == observedAfter)
                        throw std::runtime_error("Injected ordinal observation failure");
                }));
            QCOMPARE(replay.readTexture(20), before);
            QCOMPARE(boundState(replay), bindingsBefore);
            if (counter) {
                QCOMPARE(replay.readBuffer(10), beforeBuffer);
                QCOMPARE(replay.readCounter(12), beforeCounter);
            }
        }
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, replayNativePixelOrdinal(replay, event, {0, 255}));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, replayNativePixelOrdinal(replay, event, {8, 0}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replayNativePixelOrdinal(replay, event, {}, {}, 1));
        bool cancelled = false;
        QVERIFY_THROWS_EXCEPTION(std::exception, replayNativePixelOrdinal(
                                                     replay, event, {},
                                                     [&](Replay &, bool after) {
                                                         if (!after)
                                                             cancelled = true;
                                                     },
                                                     256ull * 1024 * 1024, 30000, [&] { return cancelled; }));
        std::vector<uint8_t> actual, actualBuffer;
        uint32_t actualCounter = 0;
        const auto report = replayNativePixelOrdinal(replay, event, {}, [&](Replay &r, bool after) {
            if (after) {
                actual = r.readTexture(20);
                if (counter) {
                    actualBuffer = r.readBuffer(10);
                    actualCounter = r.readCounter(12);
                }
            }
        });
        QCOMPARE(actual, expected);
        QCOMPARE(actualBuffer, expectedBuffer);
        QCOMPARE(actualCounter, expectedCounter);
        QCOMPARE(replay.readTexture(20), before);
        QCOMPARE(boundState(replay), bindingsBefore);
        if (mode == "depth") {
            QCOMPARE(replay.readTexture(720), beforeDepth);
            QCOMPARE(report.at("ordinal_selection").at("original_depth_view_bound"), Json(true));
        }
        if (counter) {
            QCOMPARE(replay.readBuffer(10), beforeBuffer);
            QCOMPARE(replay.readCounter(12), beforeCounter);
        }
        save({{"report", report},
              {"reference", expected},
              {"actual", actual},
              {"counter", actualCounter},
              {"before_counter", beforeCounter},
              {"buffer", actualBuffer},
              {"restored", true}});
    }
};
QTEST_GUILESS_MAIN(NativePixelOrdinalReplayTests)
#include "NativePixelOrdinalReplayTests.moc"
