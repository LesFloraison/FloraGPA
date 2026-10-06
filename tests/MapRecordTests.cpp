#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/CopyCommands.h"
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
    void sparseOriginals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 8; ++mode)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp))) << mode << warp;
    }
    void sparseOriginals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = qEnvironmentVariable("FLORA_SPARSE_MAP_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_SPARSE_MAP_CAPTURES to the original sparse Map corpus");
        const auto folder = root + QString("/%1/").arg(mode);
        auto bytes = [&](const QString &name) {
            QFile file(folder + "hardware/" + name);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Missing sparse Map producer oracle");
            const auto data = file.readAll();
            return Raw(reinterpret_cast<const uint8_t *>(data.data()),
                       reinterpret_cast<const uint8_t *>(data.data()) + data.size());
        };
        const auto initial = bytes("before.bin"), expected = bytes("after.bin");
        QVERIFY(initial != expected);
        Frame frame((folder + "capture.gpa_frame").toStdWString());
        const auto audit = auditMapRecords(frame);
        if (mode == 7) {
            unsigned noWaitSuccess = 0;
            for (const auto &[id, record] : audit)
                if (frame.entry(id).type == 0x34ec && record.result == 0 && record.flags == 0x100000)
                    ++noWaitSuccess;
            QCOMPARE(noWaitSuccess, 2u);
        }
        std::vector<Id> writes;
        for (const auto &[id, entry] : frame.entries()) {
            if (entry.category == 7 && entry.type == 0x246)
                writes.push_back(id);
        }
        QCOMPARE(writes.size(), size_t(2));
        const auto &first = requireMapRecord(audit, writes[0]);
        const auto &second = requireMapRecord(audit, writes[1]);
        QCOMPARE(first.resource, second.resource);
        QCOMPARE(first.kind, mode >= 5 ? 4u : 2u);
        QCOMPARE(second.kind, mode >= 5 ? 5u : 3u);
        std::vector<Id> copyTargets;
        for (const auto &[id, entry] : frame.entries())
            if (entry.category == 7 && entry.type == 0x3e) {
                const auto copy = readCopyCommand(entry.type, frame.payload(id));
                if (copy.source == first.resource)
                    copyTargets.push_back(copy.destination);
            }
        QCOMPARE(copyTargets.size(), size_t(2));
        const Id copyTarget = copyTargets.front();
        // These originals store full GenData even though the application writes
        // only four pixels. They are not evidence for texture GenDataDiff.
        for (unsigned n = 0; n < 2; ++n) {
            const auto &record = requireMapRecord(audit, writes[n]);
            QCOMPARE(frame.entry(record.data).type, uint16_t(1));
            const auto saved = frame.data(record.data);
            QCOMPARE(Raw(saved.begin(), saved.end()), n ? expected : initial);
        }
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        auto storage = [&](Replay &replay, Id resource) {
            return mode >= 4 ? replay.readBuffer(resource) : replay.readTexture(resource);
        };
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(storage(replay, first.resource), expected);
            QCOMPARE(storage(replay, copyTarget), mode >= 6 ? initial : expected);
            QCOMPARE(storage(replay, copyTargets.back()), expected);
            QCOMPARE(replay.counts.at("Map"), uint64_t(2));
            QCOMPARE(replay.counts.at("CopyResource"), uint64_t(mode >= 6 ? 4 : 2));
            QCOMPARE(replay.counts.at("map_read_synchronizations"), uint64_t(mode >= 6 ? 3 : 2));
            if (mode >= 6) {
                Raw image;
                for (unsigned pixel = 0; pixel < 64; ++pixel)
                    image.insert(image.end(), initial.begin(), initial.begin() + 4);
                QCOMPARE(replay.output().rgba, image);
            }
        }
        for (bool before : {true, false}) {
            options.until = writes[1];
            options.before = before;
            Replay prefix(frame, options);
            prefix.run();
            QCOMPARE(storage(prefix, first.resource), before ? initial : expected);
            QCOMPARE(storage(prefix, copyTarget), initial); // The second copy has not run.
        }
        options.until = 0;
        options.before = false;
        options.disabled.insert(writes[1]);
        Replay omitted(frame, options);
        omitted.run();
        QCOMPARE(storage(omitted, first.resource), initial);
        QCOMPARE(storage(omitted, copyTarget), initial);
    }
    void diffBytes_data() {
        QTest::addColumn<bool>("texture");
        QTest::addColumn<bool>("warp");
        for (bool texture : {false, true})
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(texture).arg(warp))) << texture << warp;
    }
    void diffBytes() {
        QFETCH(bool, texture);
        QFETCH(bool, warp);
        Capture c;
        c.add(1, 5, 0x127, Raw(24));
        auto desc = texture ? pack(D3D11_TEXTURE1D_DESC{8, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        D3D11_USAGE_STAGING, 0, 0x30000, 0})
                            : pack(D3D11_BUFFER_DESC{16, D3D11_USAGE_STAGING, 0, 0x30000, 0, 0});
        auto resource = pack(Id(0), Id(0));
        resource.insert(resource.end(), desc.begin(), desc.end());
        append(resource, Id(3));
        c.add(2, 5, texture ? 0x84 : 0x83, resource);
        Raw initial(texture ? 96 : 16);
        for (size_t i = 0; i < initial.size(); ++i)
            initial[i] = uint8_t(i + 1);
        auto data = word(uint32_t(initial.size()));
        data.insert(data.end(), initial.begin(), initial.end());
        c.add(3, 9, 1, data);
        // Unsorted, overlapping ranges retain file order. The final range wins
        // where it overlaps; untouched bytes and other subresources are retained.
        c.add(4, 9, 0x100, pack(24u, 12u, 8u, 4u, 0u, 4u, 2u, 4u, 0xaabbccddu, 0x11223344u, 0x55667788u));
        auto command = map(3, 0, 2, 0, 4);
        put(command, 28, texture ? 3u : 0u);
        c.add(10, 7, 0x246, command);
        auto release = unmap();
        put(release, 24, texture ? 3u : 0u);
        c.add(11, 7, 0x34ed, release);
        QTemporaryDir dir;
        const auto path = dir.filePath("diff.gpa_frame");
        c.save(path);
        QVERIFY(validateFrame(path.toStdWString())["errors"] == 0);
        Frame frame(path.toStdWString());
        auto expected = initial;
        const size_t offset = texture ? 80 : 0;
        put(expected, offset + 8, 0xaabbccddu);
        put(expected, offset, 0x11223344u);
        put(expected, offset + 2, 0x55667788u);
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run();
            QCOMPARE(texture ? replay.readTexture(2) : replay.readBuffer(2), expected);
            QCOMPARE(replay.counts.at("Map"), uint64_t(1));
        }
        options.disabled.insert(10);
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(texture ? disabled.readTexture(2) : disabled.readBuffer(2), initial);
    }
    void failedWriteHasNoDataDependency() {
        auto c = base(true);
        c.add(10, 7, 0x246, map(4, int32_t(0x887a000a), 999, 0, 0));
        QTemporaryDir dir;
        const auto path = dir.filePath("write-failed.gpa_frame");
        c.save(path);
        QVERIFY(validateFrame(path.toStdWString())["errors"] == 0);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        QVERIFY(!replay.counts.contains("Map"));
        QCOMPARE(replay.readBuffer(2), pack(10u, 20u, 30u, 40u));
    }
    void savedWriteRejections_data() {
        QTest::addColumn<QString>("mutation");
        for (const auto *name :
             {"null-data", "missing-data", "short-data", "long-data", "linked", "nonzero-success",
              "bad-flags", "read-kind", "diff-header", "diff-range", "diff-short", "diff-extra"})
            QTest::newRow(name) << QString(name);
    }
    void savedWriteRejections() {
        QFETCH(QString, mutation);
        auto c = base(true);
        auto command = map(4, 0, 2, 0, 4);
        auto data = pack(16u, 1u, 2u, 3u, 4u);
        uint16_t type = 1;
        if (mutation == "null-data")
            put(command, 40, Id(0));
        if (mutation == "missing-data")
            put(command, 40, Id(999));
        if (mutation == "short-data")
            data = pack(12u, 1u, 2u, 3u);
        if (mutation == "long-data")
            data = pack(20u, 1u, 2u, 3u, 4u, 5u);
        if (mutation == "linked")
            put(command, 0, Id(123));
        if (mutation == "nonzero-success")
            put(command, 16, 1u);
        if (mutation == "bad-flags")
            put(command, 36, 1u);
        if (mutation == "read-kind")
            put(command, 32, 1u);
        if (mutation.startsWith("diff-")) {
            type = 0x100;
            data = pack(8u, 4u, 0u, 4u, 123u);
            if (mutation == "diff-header")
                put(data, 0, 7u);
            if (mutation == "diff-range")
                put(data, 8, 15u);
            if (mutation == "diff-short")
                put(data, 12, 8u);
            if (mutation == "diff-extra")
                put(data, 12, 0u);
        }
        c.add(4, 9, type, data);
        c.add(10, 7, 0x246, command);
        QTemporaryDir dir;
        auto path = dir.filePath("write-invalid.gpa_frame");
        c.save(path);
        const auto report = validateFrame(path.toStdWString());
        QVERIFY(report["errors"].get<size_t>() > 0);
        bool located = false;
        for (const auto &finding : report["findings"])
            if (finding["kind"] == "map_write_rejected") {
                QVERIFY(finding["event_id"] == 10 && finding["resource_id"] == 2);
                QVERIFY(finding["data_id"] == (mutation == "null-data"      ? 0
                                               : mutation == "missing-data" ? 999
                                                                            : 4));
                located = true;
            }
        QVERIFY(located);
        Frame frame(path.toStdWString());
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        QVERIFY(!replay.counts.contains("Map"));
    }
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
            QCOMPARE(replay.counts.at("map_read_synchronizations"), uint64_t(1));
            QCOMPARE(replay.counts.at("unmap_observations"), uint64_t(1));
        }
        QCOMPARE(std::string(replayCapability(0x34ec).handling), std::string("execute"));
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
        ReplayOptions options;
        options.warp = true;
        Replay replay(f, options);
        replay.run();
        QVERIFY(!replay.counts.contains("map_read_synchronizations"));
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
