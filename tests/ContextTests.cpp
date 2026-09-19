#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/ContextInspector.h"
#include "core/Commands.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... values) {
    Raw out;
    (append(out, values), ...);
    return out;
}
void device(Capture &c, Id id = 80) { c.add(id, 5, 0x81, Raw(28)); }
void staging(Capture &c, Id id = 81, Id deviceId = 80) {
    c.add(id, 5, 0x85, pack(Id(0), deviceId, 1u, 1u, 1u, 1u, 28u, 1u, 0u, 3u, 0u, 0x20000u, 0u, Id(0)));
}
Raw map(Id context = 1, Id resource = 81, Id link = 0, int32_t hr = 0, uint32_t kind = 1, uint32_t flags = 0,
        Id data = 9000, uint32_t sub = 0) {
    return pack(link, context, hr, resource, sub, kind, flags, data);
}
Raw unmap(Id context = 1, Id resource = 81, Id link = 0, uint32_t sub = 0) {
    return pack(link, context, resource, sub);
}
void proof(Capture &c, Id context = 1, Id first = 82, Id resource = 81) {
    c.add(first, 7, 0x34ec, map(context, resource));
    c.add(first + 1, 7, 0x34ed, unmap(context, resource));
}
void exportFixture(const Capture &c, const QString &name) {
    auto root = qEnvironmentVariable("FLORA_CONTEXT_EVIDENCE_DIR");
    if (root.isEmpty())
        return;
    QDir().mkpath(root);
    auto copy = c;
    copy.save(root + '/' + name + ".gpa_frame");
}
Capture recoveredCompute() {
    auto c = computeCapture();
    c.entries.erase(
        std::remove_if(c.entries.begin(), c.entries.end(), [](const auto &e) { return e.id == 1; }),
        c.entries.end());
    device(c);
    staging(c);
    proof(c);
    return c;
}
} // namespace
class ContextTests final : public QObject {
    Q_OBJECT
  private slots:
    void explicitIdentity() {
        Capture c;
        Id id = 1;
        for (uint16_t type : {0x99, 0x10b, 0x121, 0x123, 0x127})
            for (uint32_t kind : {0u, 1u})
                c.add(id++, 5, type, pack(Id(0xabcdef0011223344), Id(100), kind, 42u));
        c.add(20, 5, 0x99, pack(Id(0), Id(0), 2u, 0u));
        c.add(21, 5, 0x99, Raw(23));
        c.add(22, 5, 0x83, Raw(24));
        QTemporaryDir dir;
        c.save(dir.path() + "/explicit.gpa_frame");
        Frame f((dir.path() + "/explicit.gpa_frame").toStdWString());
        for (Id n = 1; n < 11; ++n) {
            auto info = describeContext(f, n);
            QCOMPARE(*info.version, unsigned((n - 1) / 2));
            QCOMPARE(info.deferred, n % 2 == 0);
            QCOMPARE(*info.pointer, Id(0xabcdef0011223344));
            QCOMPARE(*info.flags, 42u);
            if (n % 2)
                requireImmediateContext(f, n);
            else
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireImmediateContext(f, n));
        }
        for (Id n : {20, 21, 22, 23})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, describeContext(f, n));
        QCOMPARE(inspectContexts(f)["invalid"].size(), size_t(2));
        exportFixture(c, "explicit");
    }
    void inference() {
        Capture c;
        device(c);
        staging(c);
        proof(c);
        proof(c, Id(0x100000005), 90);
        // Capture index order differs from command order; evidence still follows command IDs.
        std::reverse(c.entries.begin(), c.entries.end());
        QTemporaryDir dir;
        c.save(dir.path() + "/valid.gpa_frame");
        Frame f((dir.path() + "/valid.gpa_frame").toStdWString());
        auto context = describeContext(f, 1);
        QVERIFY(!context.deferred);
        QVERIFY(!context.version);
        QVERIFY(!context.flags);
        QVERIFY(!context.pointer);
        QCOMPARE(context.device, Id(80));
        QCOMPARE(context.evidence.size(), size_t(1));
        QCOMPARE(context.evidence[0].event, Id(82));
        QCOMPARE(context.evidence[0].unmap, Id(83));
        requireImmediateContext(f, 1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, describeContext(f, 1, false));
        QCOMPARE(f.contextRecovery().contexts.size(), size_t(2));
        exportFixture(c, "inferred");
    }
    void rejectedEvidence() {
        const QStringList names{"missing-unmap",      "wrong-resource", "unmap-link",        "overlap",
                                "malformed",          "write-map",      "erg-read-map",      "failed-map",
                                "pending-second-map", "list-parent",    "different-devices", "bad-flags",
                                "bad-subresource",    "zero-data",      "nonstaging",        "bound-staging",
                                "msaa-staging",       "wrong-device",   "unknown-kind",      "read-link",
                                "explicit-deferred"};
        for (int test = 0; test < names.size(); ++test) {
            Capture c;
            device(c);
            staging(c);
            auto read = map();
            auto release = unmap();
            if (test == 1)
                release = unmap(1, 82);
            if (test == 2)
                release = unmap(1, 81, 3);
            if (test == 4)
                release.pop_back();
            if (test == 5)
                read = map(1, 81, 0, 0, 4);
            if (test == 7)
                read = map(1, 81, 0, -1);
            if (test == 11)
                read = map(1, 81, 0, 0, 1, 2);
            if (test == 12)
                read = map(1, 81, 0, 0, 1, 0, 9000, 1);
            if (test == 13)
                read = map(1, 81, 0, 0, 1, 0, 0);
            if (test >= 14 && test <= 16) {
                auto it =
                    std::find_if(c.entries.begin(), c.entries.end(), [](auto &e) { return e.id == 81; });
                auto offset = size_t(it->offset);
                put(c.bytes,
                    offset + (test == 14   ? 44
                              : test == 15 ? 48
                                           : 36),
                    test == 14   ? 0u
                    : test == 15 ? 8u
                                 : 2u);
            }
            if (test == 17)
                c.entries[0].type = 0x82;
            if (test == 18)
                read = map(1, 81, 0, 0, 6);
            if (test == 19)
                read = map(1, 81, 100);
            if (test == 20)
                c.add(1, 5, 0x99, pack(Id(0), Id(80), 1u, 0u));
            c.add(100, 7, test == 6 ? 0x246 : 0x34ec, read);
            if (test == 3)
                c.add(101, 7, 0x34ec, read);
            if (test != 0)
                c.add(102, 7, 0x34ed, release);
            if (test == 8)
                c.add(103, 7, 0x34ec, read);
            if (test == 9)
                c.add(200, 5, 0x9a, pack(Id(0), Id(0x100000001)));
            if (test == 10) {
                device(c, 90);
                staging(c, 91, 90);
                proof(c, 1, 110, 91);
            }
            QTemporaryDir dir;
            c.save(dir.path() + "/blocked.gpa_frame");
            Frame f((dir.path() + "/blocked.gpa_frame").toStdWString());
            QVERIFY2(f.contextRecovery().contexts.empty(), qPrintable(names[test]));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, requireImmediateContext(f, 1));
            exportFixture(c, names[test]);
        }
    }
    void replayRecoveredContext_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("warp") << true;
        QTest::newRow("hardware") << false;
    }
    void replayRecoveredContext() {
        QFETCH(bool, warp);
        auto c = recoveredCompute();
        Id id = 112;
        for (uint16_t type : {0x3109, 0x3209, 0x3372, 0x3438, 0x3550})
            c.add(id++, 7, type, pack(Id(0), Id(1), 0, 1u, Id(0)));
        c.add(id++, 7, 0x3550, pack(Id(0), Id(1), int32_t(0x80004005), 0u, Id(0)));
        QTemporaryDir dir;
        c.save(dir.path() + "/replay.gpa_frame");
        Frame f((dir.path() + "/replay.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = warp;
        Replay replay(f, options);
        replay.run();
        QCOMPARE(firstWord(replay, 7), 28u);
        QCOMPARE(firstWord(replay, 13), 2u);
        QCOMPARE(replay.counts["finish_command_list_metadata"], uint64_t(6));
        auto before = f.sha256();
        validateWritableCommand(f, 111);
        QCOMPARE(f.sha256(), before);
        for (auto t : {0x3109, 0x3209, 0x3372, 0x3438, 0x3550}) {
            auto payload = pack(Id(0), Id(1), 0, 1u, Id(0));
            auto finish = readFinishCommandList(uint16_t(t), payload);
            acceptFinishCommandList(f, finish);
            finish.reference = 9;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptFinishCommandList(f, finish));
            finish.reference = 0;
            finish.hresult = 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, acceptFinishCommandList(f, finish));
        }
        if (warp)
            exportFixture(c, "replay");
    }
    void replayRejectsUnsupportedContexts() {
        // Validate the replay entry point, not only the metadata helper.
        for (int scenario = 0; scenario < 6; ++scenario) {
            auto c = recoveredCompute();
            if (scenario == 0)
                c.add(1, 5, 0x99, pack(Id(0), Id(80), 1u, 0u));
            if (scenario == 1)
                c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                               [](const auto &e) { return e.id == 83; }),
                                c.entries.end());
            if (scenario == 2 || scenario == 3)
                c.add(112, 7, 0x3550,
                      pack(Id(0), Id(1), scenario == 2 ? 1 : 0, 0u, Id(scenario == 3 ? 9 : 0)));
            if (scenario == 4 || scenario == 5)
                c.add(112, 7, scenario == 4 ? 0x41 : 0x30d1, pack(Id(0), Id(1), Id(201), 0u));
            QTemporaryDir dir;
            c.save(dir.path() + "/rejected.gpa_frame");
            Frame f((dir.path() + "/rejected.gpa_frame").toStdWString());
            ReplayOptions options;
            options.warp = true;
            Replay replay(f, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
        }
    }
    void commandListInventory() {
        Capture c;
        c.add(32, 5, 0x123, pack(Id(0x12345678), Id(80), 1u, 0u));
        c.add(201, 5, 0x9a, pack(Id(0xdeadbeef), Id(32)));
        c.add(200, 5, 0x9a, pack(Id(0xdeadbeef), Id(0x100000020)));
        c.add(202, 5, 0x9a, pack(Id(201), Id(32)));
        c.add(300, 7, 0x37, pack(Id(7), Id(0), Id(0x1000000c8), 3u, 0u));
        c.add(301, 7, 0x242, pack(Id(0), Id(201)));
        c.add(302, 7, 0x244, pack(Id(0), Id(201)));
        c.add(303, 7, 0x34e7, pack(Id(0), Id(201), Id(0), 0u, uint8_t(0)));
        c.add(304, 7, 0x41, pack(Id(0x100000003), Id(0x100000004), Id(201), 1u));
        c.add(305, 7, 0x30d1, pack(Id(0), Id(32), Id(0xdeadbeef), 0u));
        c.add(306, 7, 0x3438, pack(Id(0), Id(32), 0, 1u, Id(201)));
        QTemporaryDir dir;
        c.save(dir.path() + "/lists.gpa_frame");
        Frame f((dir.path() + "/lists.gpa_frame").toStdWString());
        auto result = inspectCommandLists(f);
        QCOMPARE(result["command_lists"][0]["id"], Json(201));
        QCOMPARE(result["command_lists"][0]["recorded_events"].size(), size_t(1));
        QCOMPARE(result["command_lists"][1]["original_player_fields"]["parent_context"], Json(32));
        QCOMPARE(result["command_lists"][1]["parent_context_type"], Json("unresolved"));
        QCOMPARE(result["command_lists"][1]["recorded_events"][0]["captured_owner"], Json(0x1000000c8ULL));
        QCOMPARE(result["execute_commands"][0]["id_candidate"], Json(201));
        QCOMPARE(result["execute_commands"][0]["pointer_candidates"], Json::array({202}));
        QVERIFY(result["execute_commands"][0]["resolved_command_list"].is_null());
        QCOMPARE(result["execute_commands"][1]["pointer_candidates"], Json::array({201, 200}));
        QCOMPARE(result["execution_supported"], Json(false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 acceptFinishCommandList(f, readFinishCommandList(0x3438, f.payload(306))));
        exportFixture(c, "lists");
    }
};
QTEST_GUILESS_MAIN(ContextTests)
#include "ContextTests.moc"
