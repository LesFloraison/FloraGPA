#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/MapRecords.h"
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
Raw map(uint32_t kind = 1, int32_t result = 0, Id resource = 2, Id parent = 0, Id data = UINT64_MAX) {
    return pack(parent, Id(1), result, resource, 0u, kind, 0u, data);
}
Raw unmap(Id resource = 2) { return pack(Id(0), Id(1), resource, 0u); }
Capture base(bool writable = false) {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    c.add(2, 5, 0x83,
          pack(Id(0), Id(0),
               D3D11_BUFFER_DESC{16, writable ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_STAGING,
                                 writable ? UINT(D3D11_BIND_VERTEX_BUFFER) : 0u,
                                 writable ? UINT(D3D11_CPU_ACCESS_WRITE) : UINT(D3D11_CPU_ACCESS_READ), 0, 0},
               Id(3)));
    c.add(3, 9, 1, pack(16u, 10u, 20u, 30u, 40u));
    return c;
}
} // namespace
class MapRecordTests final : public QObject {
    Q_OBJECT
  private slots:
    void readObservations_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void readObservations() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        auto c = base();
        c.add(10, 7, 0x34ec, map());
        c.add(11, 7, 0x34ed, unmap());
        auto path = dir.filePath("read.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto audit = auditMapRecords(f);
        QCOMPARE(requireMapRecord(audit, 10).pairedEvent, Id(11));
        QCOMPARE(requireMapRecord(audit, 11).pairedEvent, Id(10));
        // The READ's data identity intentionally has no capture data entry.
        QVERIFY(!f.entries().contains(UINT64_MAX));
        QCOMPARE(validateFrame(path.toStdWString())["errors"], nlohmann::json(0));
        ReplayOptions options;
        options.warp = warp;
        Replay replay(f, options);
        for (int i = 0; i < 2; ++i) {
            replay.run();
            QCOMPARE(replay.readBuffer(2), pack(10u, 20u, 30u, 40u));
            QCOMPARE(replay.counts.at("map_read_observations"), uint64_t(1));
            QCOMPARE(replay.counts.at("unmap_observations"), uint64_t(1));
        }
        QCOMPARE(std::string(replayCapability(0x34ec).handling), std::string("metadata"));
        QCOMPARE(std::string(replayCapability(0x34ed).handling), std::string("metadata"));
    }
    void writesStillExecute_data() { readObservations_data(); }
    void writesStillExecute() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        auto c = base(true);
        c.add(4, 9, 1, pack(16u, 1u, 2u, 3u, 4u));
        c.add(10, 7, 0x246, map(4, 0, 2, 0, 4));
        c.add(11, 7, 0x34ed, unmap());
        auto path = dir.filePath("write.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(f, options);
        replay.run();
        QCOMPARE(replay.readBuffer(2), pack(1u, 2u, 3u, 4u));
        QCOMPARE(replay.counts.at("Map"), uint64_t(1));
        QCOMPARE(replay.counts.at("unmap_observations"), uint64_t(1));
        options.disabled.insert(10);
        Replay disabled(f, options);
        disabled.run();
        QCOMPARE(disabled.readBuffer(2), pack(10u, 20u, 30u, 40u));
    }
    void rejectUnsafeObservations_data() {
        QTest::addColumn<QString>("mutation");
        for (const char *name :
             {"short", "long", "write", "linked", "missing-resource", "bad-subresource", "missing-unmap",
              "orphan-unmap", "wrong-pair", "overlap", "null-data", "bad-kind", "bad-flags",
              "nonzero-success", "deferred", "short-unmap", "long-unmap", "linked-unmap", "wrong-context"})
            QTest::newRow(name) << QString(name);
    }
    void rejectUnsafeObservations() {
        QFETCH(QString, mutation);
        auto c = base();
        auto read = map();
        auto release = unmap();
        if (mutation == "short")
            read.pop_back();
        if (mutation == "long")
            read.push_back(0);
        if (mutation == "write")
            read = map(4);
        if (mutation == "linked")
            read = map(1, 0, 2, 123);
        if (mutation == "missing-resource")
            read = map(1, 0, 999);
        if (mutation == "bad-subresource")
            put(read, 28, 1u);
        if (mutation == "wrong-pair")
            release = unmap(999);
        if (mutation == "short-unmap")
            release.pop_back();
        if (mutation == "long-unmap")
            release.push_back(0);
        if (mutation == "linked-unmap")
            put(release, 0, Id(123));
        if (mutation == "wrong-context")
            put(release, 8, Id(999));
        if (mutation == "null-data")
            read = map(1, 0, 2, 0, 0);
        if (mutation == "bad-kind")
            read = map(7);
        if (mutation == "bad-flags")
            put(read, 36, 1u);
        if (mutation == "nonzero-success")
            read = map(1, 1);
        if (mutation == "deferred")
            put(c.bytes, size_t(c.entries.front().offset) + 16, 1u);
        if (mutation != "orphan-unmap")
            c.add(10, 7, 0x34ec, read);
        if (mutation == "overlap")
            c.add(12, 7, 0x34ec, read);
        if (mutation != "missing-unmap")
            c.add(20, 7, 0x34ed, release);
        QTemporaryDir dir;
        auto path = dir.filePath("bad.gpa_frame");
        c.save(path);
        const auto report = validateFrame(path.toStdWString());
        QCOMPARE(report["status"], nlohmann::json("blocked"));
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(f, options);
        QVERIFY_EXCEPTION_THROWN(replay.run(), std::runtime_error);
        bool located = false;
        for (auto &finding : report["findings"])
            if (finding["kind"] == "map_observation_rejected") {
                QVERIFY(!finding["event_id"].is_null());
                QVERIFY(!finding["resource_id"].is_null());
                located = true;
            }
        QVERIFY(located);
    }
    void failedMapHasNoStorageEffect() {
        auto c = base();
        c.add(10, 7, 0x34ec, map(1, int32_t(0x887a000a), 999, 0, 0));
        QTemporaryDir dir;
        auto path = dir.filePath("failed.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        const auto record = requireMapRecord(auditMapRecords(f), 10);
        QCOMPARE(record.pairedEvent, Id(0));
        QVERIFY(record.result < 0);
        QCOMPARE(validateFrame(path.toStdWString())["errors"], nlohmann::json(0));
    }
    void prefixAndPayloadOverride() {
        auto c = base();
        c.add(10, 7, 0x34ec, map());
        c.add(20, 7, 0x34ed, unmap());
        c.add(30, 7, 0x34ed, unmap()); // Later orphan must not invalidate the earlier valid pair.
        QTemporaryDir dir;
        auto path = dir.filePath("prefix.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 10;
        Replay prefix(f, options);
        prefix.run();
        QCOMPARE(prefix.counts.at("map_read_observations"), uint64_t(1));
        QCOMPARE(prefix.readBuffer(2), pack(10u, 20u, 30u, 40u));
        options.commandPayloads[10] = map(4);
        Replay edited(f, options);
        QVERIFY_EXCEPTION_THROWN(edited.run(), std::runtime_error);
        QCOMPARE(validateFrame(path.toStdWString())["status"], nlohmann::json("blocked"));
    }
};
QTEST_GUILESS_MAIN(MapRecordTests)
#include "MapRecordTests.moc"
