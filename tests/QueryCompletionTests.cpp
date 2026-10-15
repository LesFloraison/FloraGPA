#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/CopyCommands.h"
#include "core/QueryCompletion.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... v) {
    Raw out;
    (append(out, v), ...);
    return out;
}
Capture base() {
    Capture c;
    c.add(1, 5, 0x127, Raw(24));
    c.add(2, 5, 0x96, pack(Id(0), Id(0), 5u, 0u));
    c.add(10, 7, 0x241, pack(Id(0), Id(1), Id(2)));
    c.add(20, 7, 0x243, pack(Id(0), Id(1), Id(2)));
    return c;
}
Raw get(int32_t hr = 0, Id query = 2, bool data = true, uint32_t flags = 0) {
    auto raw = pack(Id(0), Id(1), hr, query, uint8_t(data));
    if (data)
        append(raw, 0u);
    append(raw, data ? 4u : 0u);
    append(raw, flags);
    return raw;
}
} // namespace
class QueryCompletionTests final : public QObject {
    Q_OBJECT
  private slots:
    void ordinaryEndObservations() {
        auto c=base();
        c.add(5,7,0x3152,pack(Id(0),Id(99),uint8_t(1),0u,0u));
        c.add(15,7,0x34fa,pack(Id(0),Id(1),Id(99)));
        c.add(30,7,0x34fb,get());
        c.add(31,7,0x34fb,get(0,99,false));
        QTemporaryDir dir;const auto path=dir.filePath("ordinary.gpa_frame");c.save(path);Frame f(path.toStdWString());
        QCOMPARE(auditEventEndObservations(f),std::set<Id>{15});
        QCOMPARE(validateFrame(f.path())["errors"],nlohmann::json(0));
        const auto audit=auditQueryCompletions(f);
        QCOMPARE(requireQueryCompletion(audit,30).end,Id(20));
        QCOMPARE(requireQueryCompletion(audit,31).end,Id(15));
        QVERIFY(requireQueryCompletion(audit,31).ordinary);
        ReplayOptions o;o.warp=true;Replay replay(f,o);replay.run();
        QCOMPARE(replay.counts.at("query_end_observations"),uint64_t(1));
        QCOMPARE(replay.counts.at("query_completion_synchronizations"),uint64_t(2));
        QCOMPARE(replay.counts.at("ordinary_query_completion_synchronizations"),uint64_t(1));
        QVERIFY(replay.unresolvedQueryCompletions().empty());
        replay.run();
        QCOMPARE(replay.counts.at("ordinary_query_completion_synchronizations"),uint64_t(1));
        o.disabled.insert(15);Replay disabled(f,o);QVERIFY_THROWS_EXCEPTION(std::runtime_error,disabled.run());
    }
    void ordinaryEndGuards_data() {
        QTest::addColumn<QString>("mode");
        for(const auto mode:{"absent","future","flags","other-type","nested","conflict","erg","deferred"})QTest::newRow(mode)<<QString(mode);
    }
    void ordinaryCompletionGuards_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"event", "timestamp", "disjoint", "size", "false-event", "begin-again", "conflict-after", "other-context", "latest-end"})
            QTest::newRow(mode) << QString(mode);
    }
    void ordinaryCompletionGuards() {
        QFETCH(QString,mode); auto c=base();
        const uint32_t type=mode=="timestamp"?2u:(mode=="disjoint" || mode=="begin-again")?3u:0u;
        c.add(5,7,0x3152,pack(Id(0),Id(99),uint8_t(1),type,0u));
        if(type==3)c.add(12,7,0x34f9,pack(Id(0),Id(1),Id(99)));
        c.add(15,7,0x34fa,pack(Id(0),Id(1),Id(99)));
        auto raw=get(0,99,true);put(raw,29,1u);put(raw,33,type==0?4u:type==2?8u:16u);
        if(mode=="size")put(raw,33,8u);
        if(mode=="false-event")put(raw,29,0u);
        if(mode=="begin-again")c.add(25,7,0x34f9,pack(Id(0),Id(1),Id(99)));
        if(mode=="conflict-after")c.add(25,7,0x3152,pack(Id(0),Id(99),uint8_t(1),2u,0u));
        if(mode=="other-context"){c.add(3,5,0x127,Raw(24));put(raw,8,Id(3));}
        if(mode=="latest-end")c.add(25,7,0x34fa,pack(Id(0),Id(1),Id(99)));
        c.add(30,7,0x34fb,raw);
        QTemporaryDir dir;const auto path=dir.filePath("completion.gpa_frame");c.save(path);Frame f(path.toStdWString());
        const auto audit=auditQueryCompletions(f);const auto& q=audit.at(30);
        if(mode=="size" || mode=="false-event")QVERIFY(!q.error.empty());
        else if(mode=="begin-again" || mode=="conflict-after" || mode=="other-context"){
            QCOMPARE(q.end,Id(0));QVERIFY(!q.missing.empty());
        }else{
            QVERIFY(q.error.empty());QVERIFY(q.ordinary);QCOMPARE(q.end,Id(mode=="latest-end"?25:15));
            ReplayOptions o;o.warp=true;Replay r(f,o);r.run();
            QCOMPARE(r.counts.at("ordinary_query_completion_synchronizations"),uint64_t(1));
            o.disabled.insert(q.end);Replay disabled(f,o);QVERIFY_THROWS_EXCEPTION(std::runtime_error,disabled.run());
        }
    }
    void timingObservations() {
        auto c=base();
        c.add(5,7,0x3152,pack(Id(0),Id(98),uint8_t(1),2u,0u));
        c.add(6,7,0x3152,pack(Id(0),Id(99),uint8_t(1),3u,0u));
        c.add(12,7,0x34f9,pack(Id(0),Id(1),Id(99)));
        c.add(13,7,0x34fa,pack(Id(0),Id(1),Id(98)));
        c.add(14,7,0x34fa,pack(Id(0),Id(1),Id(99)));
        QTemporaryDir dir;const auto path=dir.filePath("timing.gpa_frame");c.save(path);Frame f(path.toStdWString());
        QCOMPARE(auditEventEndObservations(f),(std::set<Id>{12,13,14}));
        QCOMPARE(validateFrame(f.path())["errors"],nlohmann::json(0));
        ReplayOptions o;o.warp=true;Replay replay(f,o);replay.run();
        QCOMPARE(replay.counts.at("query_begin_observations"),uint64_t(1));
        QCOMPARE(replay.counts.at("query_end_observations"),uint64_t(2));
        // A TIMESTAMP is End-only; descriptor evidence must not authorize Begin.
        c.add(11,7,0x34f9,pack(Id(0),Id(1),Id(98)));
        const auto bad=dir.filePath("invalid-timing.gpa_frame");c.save(bad);Frame invalid(bad.toStdWString());
        QVERIFY(!auditEventEndObservations(invalid).contains(11));
        Replay reject(invalid,o);QVERIFY_THROWS_EXCEPTION(std::runtime_error,reject.run());
    }
    void ordinaryEndGuards() {
        QFETCH(QString,mode);auto c=base();
        if(mode!="absent")c.add(mode=="future"?25:5,7,0x3152,pack(Id(0),Id(99),uint8_t(1),mode=="other-type"?1u:0u,mode=="flags"?1u:0u));
        if(mode=="conflict")c.add(6,7,0x3152,pack(Id(0),Id(99),uint8_t(1),2u,0u));
        if(mode=="deferred"){auto raw=Raw(24);put<uint32_t>(raw,16,1);c.add(7,5,0x127,raw);}
        c.add(15,7,mode=="erg"?0x243:0x34fa,pack(Id(mode=="nested"?10:0),Id(mode=="deferred"?7:1),Id(99)));
        QTemporaryDir dir;const auto path=dir.filePath("guard.gpa_frame");c.save(path);Frame f(path.toStdWString());
        if(mode=="deferred")QVERIFY_THROWS_EXCEPTION(std::runtime_error,auditEventEndObservations(f));
        else QVERIFY(auditEventEndObservations(f).empty());
        ReplayOptions o;o.warp=true;Replay replay(f,o);QVERIFY_THROWS_EXCEPTION(std::runtime_error,replay.run());
    }
    void versions_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("statusOnly");
        for (int type : {0x30b4, 0x31b4, 0x331d, 0x33e3, 0x34fb})
            for (bool statusOnly : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(statusOnly))) << type << statusOnly;
    }
    void versions() {
        QFETCH(int, type);
        QFETCH(bool, statusOnly);
        auto c = base();
        c.add(30, 7, uint16_t(type), get(0, 2, !statusOnly, statusOnly ? 1 : 0));
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        const auto audit = auditQueryCompletions(f);
        QCOMPARE(requireQueryCompletion(audit, 30).end, Id(20));
        ReplayOptions o;
        o.warp = true;
        Replay r(f, o);
        for (int n = 0; n < 2; ++n) {
            r.run();
            QCOMPARE(r.counts.at("query_completion_synchronizations"), uint64_t(1));
            QVERIFY(r.unresolvedQueryCompletions().empty());
        }
        o.disabled.insert(20);
        Replay disabled(f, o);
        QVERIFY_EXCEPTION_THROWN(disabled.run(), std::runtime_error);
        o.disabled.clear();
        o.commandPayloads[30] = get(1);
        Replay edited(f, o);
        QVERIFY_EXCEPTION_THROWN(edited.run(), std::runtime_error);
    }
    void boundaries_data() {
        QTest::addColumn<QString>("mode");
        for (auto mode :
             {"failed", "pending", "missing", "no-begin", "active", "future-end", "nested-interval",
              "wrong-context", "flags", "size", "null-data", "link", "status", "non-query"})
            QTest::newRow(mode) << QString(mode);
    }
    void boundaries() {
        QFETCH(QString, mode);
        auto c = base();
        auto raw = get();
        if (mode == "failed")
            raw = get(int32_t(0x80004005), 999);
        if (mode == "pending")
            raw = get(1, 999);
        if (mode == "missing")
            raw = get(0, 999);
        if (mode == "non-query")
            raw = get(0, 1);
        if (mode == "no-begin")
            c.entries.erase(c.entries.begin() + 2);
        if (mode == "active" || mode == "future-end") {
            c.entries.erase(c.entries.begin() + 3);
            if (mode == "future-end")
                c.add(40, 7, 0x243, pack(Id(0), Id(1), Id(2)));
        }
        if (mode == "nested-interval")
            c.add(25, 7, 0x241, pack(Id(123), Id(1), Id(2)));
        if (mode == "wrong-context")
            put(raw, 8, Id(999));
        if (mode == "flags")
            put(raw, 37, 2u);
        if (mode == "size")
            put(raw, 33, 8u);
        if (mode == "null-data") {
            raw = get(0, 2, false);
            put(raw, 29, 4u);
        }
        if (mode == "link")
            put(raw, 0, Id(10));
        if (mode == "status")
            put(raw, 16, int32_t(2));
        c.add(30, 7, 0x34fb, raw);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        const auto audit = auditQueryCompletions(f);
        const auto &q = audit.at(30);
        QCOMPARE(q.end, Id(0));
        const bool missing = mode == "missing" || mode == "no-begin" || mode == "active" ||
                             mode == "future-end" || mode == "nested-interval";
        QCOMPARE(!q.missing.empty(), missing);
        const bool failed = mode == "failed" || mode == "pending";
        QCOMPARE(!q.error.empty(), !missing && !failed);
        if (!missing && !failed)
            QVERIFY_EXCEPTION_THROWN(requireQueryCompletion(audit, 30), std::runtime_error);
        const auto report = validateFrame(f.path());
        bool found = false;
        for (const auto &item : report["findings"])
            if (item["event_id"] == 30 &&
                item["kind"] == (missing ? "query_completion_not_saved" : "query_completion_rejected"))
                found = true;
        if (!failed)
            QVERIFY(found);
        if (mode == "failed" || mode == "pending" || mode == "missing") {
            ReplayOptions o;
            o.warp = true;
            Replay r(f, o);
            r.run();
            QVERIFY(!r.counts.contains("query_completion_synchronizations"));
            QCOMPARE(r.unresolvedQueryCompletions().size(), size_t(missing));
        }
    }
    void malformedAndCancel() {
        const auto raw = get();
        for (size_t size = 0; size < raw.size(); ++size)
            QVERIFY_EXCEPTION_THROWN(readQueryGetData(Bytes(raw).first(size)), std::runtime_error);
        auto trailing = raw;
        trailing.push_back(0);
        QVERIFY_EXCEPTION_THROWN(readQueryGetData(trailing), std::runtime_error);
        auto presence = raw;
        presence[28] = 2;
        QVERIFY_EXCEPTION_THROWN(readQueryGetData(presence), std::runtime_error);
        auto c = base();
        c.add(30, 7, 0x34fb, raw);
        QTemporaryDir dir;
        c.save(dir.filePath("frame.gpa_frame"));
        Frame f(dir.filePath("frame.gpa_frame").toStdWString());
        unsigned checks = 0;
        auditQueryCompletions(f, [&] {
            ++checks;
            return false;
        });
        for (unsigned stop = 1; stop <= checks; ++stop) {
            unsigned at = 0;
            QVERIFY_EXCEPTION_THROWN(auditQueryCompletions(f, [&] { return ++at == stop; }),
                                     OperationCancelled);
            QCOMPARE(requireQueryCompletion(auditQueryCompletions(f), 30).end, Id(20));
        }
        checks = 0;
        const auto baseline = validateFrame(f.path(), [&] {
            ++checks;
            return false;
        });
        for (unsigned stop = 1; stop <= checks; ++stop) {
            unsigned at = 0;
            const auto cancelled = validateFrame(f.path(), [&] { return ++at == stop; });
            QVERIFY(cancelled["cancelled"].get<bool>());
            QCOMPARE(validateFrame(f.path()), baseline);
        }
        qInfo() << "Query preflight interruption positions" << checks;
    }
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode : {8, 9, 10})
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp))) << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = qEnvironmentVariable("FLORA_QUERY_SYNC_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_QUERY_SYNC_CAPTURES to original completion captures");
        const auto folder = root + '/' + QString::number(mode);
        Frame f((folder + "/capture.gpa_frame").toStdWString());
        const auto audit = auditQueryCompletions(f);
        unsigned success = 0;
        Id completion = 0;
        for (const auto &[id, q] : audit)
            if (q.result == 0) {
                ++success;
                completion = id;
                QVERIFY(q.error.empty());
                QCOMPARE(q.end != 0, mode == 10);
                QCOMPARE(!q.missing.empty(), mode != 10);
            }
        QCOMPARE(success, 1u);
        ReplayOptions o;
        o.warp = warp;
        Replay r(f, o);
        for (int repeat = 0; repeat < 2; ++repeat) {
            r.run();
            QCOMPARE(r.unresolvedQueryCompletions().size(), size_t(mode != 10));
            if (mode == 10) {
                QFile file(folder + "/hardware/image-0.rgba");
                QVERIFY(file.open(QIODevice::ReadOnly));
                const auto expected = file.readAll();
                const auto actual = r.output().rgba;
                QCOMPARE(QByteArray(reinterpret_cast<const char *>(actual.data()), actual.size()), expected);
                QCOMPARE(r.counts.at("query_completion_synchronizations"), uint64_t(1));
            }
        }
        if (mode == 10) {
            const auto writes = auditMapRecords(f);
            Id second = 0, source = 0, target = 0;
            for (const auto &[id, entry] : f.entries()) {
                if (entry.category == 7 && entry.type == 0x246 && id > completion) {
                    second = id;
                    source = requireMapRecord(writes, id).resource;
                    break;
                }
            }
            for (const auto &[id, entry] : f.entries())
                if (entry.category == 7 && entry.type == 0x3e && id < completion) {
                    const auto copy = readCopyCommand(entry.type, f.payload(id));
                    if (copy.source == source)
                        target = copy.destination;
                }
            QVERIFY(second && target);
            o.until = second;
            Replay prefix(f, o);
            prefix.run();
            QFile file(folder + "/hardware/before.bin");
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto expected = file.readAll();
            const auto actual = prefix.readBuffer(target);
            QCOMPARE(QByteArray(reinterpret_cast<const char *>(actual.data()), actual.size()), expected);
        }
    }
};
QTEST_GUILESS_MAIN(QueryCompletionTests)
#include "QueryCompletionTests.moc"
