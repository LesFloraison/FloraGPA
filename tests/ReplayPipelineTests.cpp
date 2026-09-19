#include "StateCapture.h"
#include "application/ReplayPipeline.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Json field(const Json &state, const char *key) {
    for (const auto &row : state.at("fields"))
        if (row.at("field") == key)
            return row;
    throw std::runtime_error(std::string("Missing field: ") + key);
}
Capture fixture() {
    auto capture = computeCapture();
    capture.add(105, 7, 0x244, statePack(Id(0), Id(1)));
    capture.add(112, 7, 0x242, statePack(Id(0), Id(1)));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    for (Id id : {20, 21})
        capture.add(id, 5, 0x88, statePack(Id(0), Id(0), sampler));
    capture.add(95, 7, 0x34f8, statePack(Id(0), Id(1), 0u, 2u, uint8_t(1), Id(20), Id(21)));
    capture.add(96, 7, 0x3522, statePack(Id(0), Id(1), 63u, 1u, uint8_t(1), Id(9), uint8_t(0)));
    capture.add(97, 7, 0x3500,
                statePack(Id(0), Id(1), 0u, uint8_t(0), Id(0), 63u, 1u, uint8_t(1), Id(9), uint8_t(0)));
    return capture;
}
void exportFixture(Capture &capture) {
    auto dir = qEnvironmentVariable("FLORA_PIPELINE_ARTIFACT_DIR");
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        capture.save(dir + "/compute.gpa_frame");
    }
}
} // namespace
class ReplayPipelineTests final : public QObject {
    Q_OBJECT
  private slots:
    void boundaries_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void boundaries() {
        QFETCH(bool, warp);
        auto capture = fixture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/compute.gpa_frame");
        Frame frame((dir.path() + "/compute.gpa_frame").toStdWString());
        for (Id event : {95, 96, 97, 100, 105, 110, 112})
            for (bool before : {true, false}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = event;
                options.before = before;
                if (event >= 100) {
                    options.buffers[100][2] = {{0, word(20)}};
                    options.buffers[100][4] = {{0, word(70)}};
                }
                Replay replay(frame, options);
                auto report = inspectReplayPipeline(frame, replay, event >= 100);
                QCOMPARE(report["fields"].size(), size_t(1306));
                QCOMPARE(report["known_fields"], Json(1302));
                QCOMPARE(report["unknown_fields"], Json(4));
                for (const auto &row : report["fields"]) {
                    QCOMPARE(row["source"]["event"], Json(event));
                    if (row["known"] == false)
                        QVERIFY(row["field"].get<std::string>().starts_with("so.offsets."));
                }
                if (event == 95 && !before) {
                    auto a = field(report, "vs.samplers.0"), b = field(report, "vs.samplers.1");
                    QCOMPARE(a["value"], b["value"]);
                    QCOMPARE(a["object"]["captured_ids"], Json({20, 21}));
                    QVERIFY(a["resource_id"].is_null());
                    QCOMPARE(a["object"]["descriptor"]["address_u"], Json(3));
                }
                if (event == 96 && !before)
                    QCOMPARE(field(report, "cs.uav.63")["value"], Json(9));
                if (event == 97 && !before)
                    QCOMPARE(field(report, "om.uav.63")["value"], Json(9));
                if (event == 100 || event == 105) {
                    QCOMPARE(field(report, "cs.cb.0")["resource_id"], Json(2));
                    QCOMPARE(field(report, "cs.srv.0")["resource_id"], Json(6));
                    QCOMPARE(field(report, "cs.cb_range.0")["value"], Json({0, 4096}));
                    QCOMPARE(firstWord(replay, 2), 2u);
                    QCOMPARE(firstWord(replay, 4), 7u);
                    QCOMPARE(firstWord(replay, 7), event == 100 && before ? 10u : 100u);
                    QCOMPARE(replay.counts["Dispatch"], event == 100 && before ? uint64_t(0) : uint64_t(1));
                }
                if (event == 110)
                    QCOMPARE(firstWord(replay, 7), before ? 100u : 109u);
                if (event == 112 && !before) {
                    QCOMPARE(field(report, "cs.shader")["value"], Json(0));
                    QCOMPARE(field(report, "rasterizer.descriptor")["value"]["cull_mode"], Json(3));
                    QCOMPARE(field(report, "depth_state.descriptor")["value"]["depth_enable"], Json(true));
                }
                auto again = inspectReplayPipeline(frame, replay, event >= 100);
                QCOMPARE(again, report);
            }
        exportFixture(capture);
    }
    void disabledAndRollback() {
        auto capture = fixture();
        QTemporaryDir dir;
        capture.save(dir.path() + "/disabled.gpa_frame");
        Frame frame((dir.path() + "/disabled.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 100;
        options.disabled.insert(100);
        options.buffers[100][2] = {{0, word(20)}};
        options.buffers[100][7] = {{0, word(1000)}};
        Replay disabled(frame, options);
        auto report = inspectReplayPipeline(frame, disabled, true);
        QCOMPARE(report["command_enabled"], Json(false));
        QCOMPARE(field(report, "cs.cb.0")["resource_id"], Json(2));
        QCOMPARE(firstWord(disabled, 7), 10u);
        QVERIFY(!disabled.counts.contains("Dispatch"));
        options.disabled.clear();
        Replay throwing(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, throwing.run({}, [](Id, bool, auto *, const auto &) {
            throw std::runtime_error("Observer failure");
        }));
        QCOMPARE(firstWord(throwing, 2), 2u);
        QCOMPARE(firstWord(throwing, 7), 10u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 throwing.inspectNativeState([](auto *, const auto &) {}));
        throwing.run();
        QCOMPARE(firstWord(throwing, 7), 1027u);
    }
    void invalidBoundary() {
        auto capture = fixture();
        capture.add(113, 7, 0x34ef, statePack(Id(0), Id(1), Id(999)));
        capture.add(114, 7, 0x327d, statePack(Id(0), Id(0xabcdef), 0u));
        QTemporaryDir dir;
        capture.save(dir.path() + "/invalid.gpa_frame");
        Frame frame((dir.path() + "/invalid.gpa_frame").toStdWString());
        for (Id event : {0, 1, 113, 114}) {
            ReplayOptions options;
            options.warp = true;
            options.until = event;
            Replay replay(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectReplayPipeline(frame, replay));
        }
    }
};
QTEST_GUILESS_MAIN(ReplayPipelineTests)
#include "ReplayPipelineTests.moc"
