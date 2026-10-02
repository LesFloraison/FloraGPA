#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/InspectionRecords.h"
#include "core/ReplayCapabilities.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... values) {
    Raw out;
    (append(out, values), ...);
    return out;
}
std::vector<std::pair<uint16_t, Raw>> records() {
    std::vector<std::pair<uint16_t, Raw>> out;
    auto add = [&](uint16_t type, Raw tail) {
        auto raw = pack(Id(UINT64_MAX - 1), Id(UINT64_MAX));
        raw.insert(raw.end(), tail.begin(), tail.end());
        out.emplace_back(type, std::move(raw));
    };
    for (uint16_t type : {0x3012, 0x324f, 0x3256, 0x3575}) {
        add(type, pack(0, Id(UINT64_MAX), Id(0), Id(UINT64_MAX)));
        add(type, pack(int32_t(0x80004002), Id(0), Id(0), Id(0)));
    }
    for (uint16_t type : {0x3013, 0x3014, 0x3250, 0x3251, 0x3576, 0x3577}) {
        add(type, pack(0u));
        add(type, pack(UINT32_MAX));
    }
    for (uint16_t type : {0x3019, 0x3146}) {
        add(type, pack(uint8_t(0)));
        add(type, pack(uint8_t(1), 3u));
    }
    add(0x302e, pack(uint8_t(0)));
    add(0x302e, pack(uint8_t(1), 28u, 4u, 0u, 1u, UINT32_MAX, 0u));
    add(0x3261, pack(int32_t(0x80004005), uint8_t(0)));
    add(0x3261, pack(0, uint8_t(1), std::array<uint8_t, 72>{}));
    add(0x3597, pack(int32_t(0x887a0002), Id(0), Id(0), uint8_t(0), Id(UINT64_MAX)));
    add(0x3597, pack(0, Id(0), Id(0), uint8_t(1), UINT32_MAX, Id(UINT64_MAX)));
    return out;
}
} // namespace
class ObjectObservationTests final : public QObject {
    Q_OBJECT
  private slots:
    void layoutsAndLengthRejections() {
        std::set<uint16_t> types;
        for (const auto &[type, raw] : records()) {
            types.insert(type);
            QVERIFY(isPassiveObjectRecord(type));
            QCOMPARE(std::string(replayCapability(type).handling), std::string("metadata"));
            QVERIFY(acceptPassiveObjectRecord(type, raw));
            for (size_t size = 0; size < raw.size(); ++size)
                QVERIFY_EXCEPTION_THROWN(acceptPassiveObjectRecord(type, Bytes(raw).first(size)),
                                         std::runtime_error);
            auto extra = raw;
            extra.push_back(0);
            QVERIFY_EXCEPTION_THROWN(acceptPassiveObjectRecord(type, extra), std::runtime_error);
        }
        QCOMPARE(types.size(), size_t(15));
        // Present and object mutations are deliberately outside this passive family.
        for (uint16_t type : {0x3257, 0x3578, 0x3017, 0xffff})
            QVERIFY(!acceptPassiveObjectRecord(type, {}));
    }
    void malformedOptionalFlags() {
        for (const auto &[type, original] : records()) {
            size_t flag = 0;
            if (type == 0x3019 || type == 0x3146 || type == 0x302e)
                flag = 16;
            if (type == 0x3261)
                flag = 20;
            if (type == 0x3597)
                flag = 36;
            if (!flag)
                continue;
            auto raw = original;
            raw[flag] = 2;
            QVERIFY_EXCEPTION_THROWN(acceptPassiveObjectRecord(type, raw), std::runtime_error);
        }
    }
    void replayKeepsStorageAndPipeline_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void replayKeepsStorageAndPipeline() {
        QFETCH(bool, warp);
        auto c = computeCapture();
        const auto samples = records();
        Id id = 1000;
        for (const auto &[type, raw] : samples)
            c.add(id++, 7, type, raw);
        QTemporaryDir dir;
        const auto path = dir.filePath("observations.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(f, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(firstWord(replay, 7), 28u);
            QCOMPARE(firstWord(replay, 13), 2u);
            QCOMPARE(replay.counts.at("Dispatch"), uint64_t(2));
            QCOMPARE(replay.counts.at("object_observation_records"), uint64_t(samples.size()));
            replay.inspectNativeState([&](ID3D11DeviceContext *context, const auto &) {
                Com<ID3D11ComputeShader> shader;
                context->CSGetShader(&shader, nullptr, nullptr);
                QVERIFY(shader);
            });
        }
        const auto report = validateFrame(path.toStdWString());
        QCOMPARE(report["errors"], nlohmann::json(0));
        for (auto &finding : report["findings"])
            QVERIFY(finding["kind"] != "auxiliary_audit");
    }
    void malformedRecordsFailProductionAndPreflight() {
        QTemporaryDir dir;
        for (const auto &[type, original] : records()) {
            Capture c;
            auto raw = original;
            raw.push_back(0); // Inspector previously marked this partial; fallback silently accepted it.
            c.add(900, 7, type, raw);
            const auto path = dir.filePath("bad.gpa_frame");
            c.save(path);
            const auto report = validateFrame(path.toStdWString());
            QCOMPARE(report["status"], nlohmann::json("blocked"));
            QCOMPARE(report["findings"].back()["event_id"], nlohmann::json(900));
            Frame f(path.toStdWString());
            ReplayOptions options;
            options.warp = true;
            Replay replay(f, options);
            QVERIFY_EXCEPTION_THROWN(replay.run(), std::runtime_error);
        }
    }
};
QTEST_GUILESS_MAIN(ObjectObservationTests)
#include "ObjectObservationTests.moc"
