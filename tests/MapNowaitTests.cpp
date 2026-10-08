#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/CopyCommands.h"
#include "core/MapRecords.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
Raw bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read original NOWAIT oracle");
    const auto data = file.readAll();
    return {data.begin(), data.end()};
}
std::filesystem::path capture(int mode) {
    return (qEnvironmentVariable("FLORA_MAP_NOWAIT_CAPTURES") + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString();
}
}
class MapNowaitTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode"); QTest::addColumn<bool>("warp");
        for (int mode=0; mode<8; ++mode) for (bool warp : {false,true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp?"warp":"hardware"))) << mode << warp;
    }
    void originals() {
        if (qEnvironmentVariableIsEmpty("FLORA_MAP_NOWAIT_CAPTURES")) QSKIP("Set original NOWAIT corpus");
        QFETCH(int,mode); QFETCH(bool,warp);
        Frame frame(capture(mode));
        const auto root=qEnvironmentVariable("FLORA_MAP_NOWAIT_CAPTURES");
        const auto manifest=nlohmann::json::parse(bytes(root+"/manifest.json"));
        bool verified=false;
        for (const auto &row:manifest["cases"]) if(row["mode"]==mode) {QCOMPARE(frame.sha256(),row["sha256"].get<std::string>()); verified=true;}
        QVERIFY(verified);
        const auto prefix=root+QString("/%1/hardware/").arg(mode);
        const auto initial=bytes(prefix+"before.bin"),expected=bytes(prefix+"expected.bin");
        QVERIFY(initial!=expected);
        const auto audit=auditMapRecords(frame); Id write=0,target=0,copy=0; unsigned failed=0;
        for (const auto &[id,record]:audit) if(frame.entry(id).type==0x246) {
            QCOMPARE(record.flags,0x100000u); QVERIFY(record.error.empty());
            if(record.result<0) {QCOMPARE(record.result,int32_t(0x887a000a)); ++failed;}
            else {QVERIFY(!write); write=id;target=record.resource;}
        }
        QVERIFY(write&&target&&failed);
        for(const auto &[id,e]:frame.entries()) if(id>write&&e.category==7&&e.type==0x3e) {
            const auto command=readCopyCommand(e.type,frame.payload(id));
            if(command.source==target)copy=command.destination;
        }
        QVERIFY(copy);
        QCOMPARE(frame.entry(audit.at(write).data).type,uint16_t(mode%4==0?0x100:1));
        QCOMPARE(validateFrame(frame.path())["errors"],nlohmann::json(0));
        auto storage=[&](Replay &replay,Id id){return mode%4==0?replay.readBuffer(id):replay.readTexture(id);};
        ReplayOptions options;options.warp=warp;Replay replay(frame,options);
        // Repeated full runs do not inspect or wait between the queued copies
        // and the successful saved Map. Resource readback happens only after it.
        for(unsigned repeat=0;repeat<8;++repeat) {
            replay.run();
            QCOMPARE(storage(replay,target),expected); QCOMPARE(storage(replay,copy),expected);
            QCOMPARE(replay.counts.at("Map"),uint64_t(1));
            QCOMPARE(replay.counts.at("map_write_readiness_waits"),uint64_t(1));
        }
        for(bool before:{true,false}) {
            options.until=write;options.before=before;Replay boundary(frame,options);boundary.run();
            QCOMPARE(storage(boundary,target),before?initial:expected);
        }
        options.until=0;options.before=false;options.disabled.insert(write);
        Replay disabled(frame,options);disabled.run();
        QCOMPARE(storage(disabled,target),initial);QCOMPARE(storage(disabled,copy),initial);
        QVERIFY(!disabled.counts.contains("Map")); QVERIFY(!disabled.counts.contains("map_write_readiness_waits"));
    }
    void malformed_data() {
        QTest::addColumn<QString>("kind");
        for(const auto *kind:{"discard","no-overwrite","flags","short","long","missing-data","short-data","long-data","range"})
            QTest::newRow(kind)<<QString(kind);
    }
    void malformed() {
        if(qEnvironmentVariableIsEmpty("FLORA_MAP_NOWAIT_CAPTURES"))QSKIP("Set original NOWAIT corpus");
        QFETCH(QString,kind);
        Frame original(capture(kind=="range"?4:2));Id event=0,data=0,resource=0;
        for(const auto &[id,r]:auditMapRecords(original)) if(original.entry(id).type==0x246&&r.result==0) {event=id;data=r.data;resource=r.resource;}
        QVERIFY(event&&data&&resource);Capture changed;
        for(const auto &[id,e]:original.entries()) {
            const auto p=original.payload(id);Raw raw(p.begin(),p.end());
            if(id==event) {
                if(kind=="discard")put(raw,32,4u);
                if(kind=="no-overwrite")put(raw,32,5u);
                if(kind=="flags")put(raw,36,0x100001u);
                if(kind=="short")raw.pop_back();
                if(kind=="long")raw.push_back(0);
                if(kind=="missing-data")put(raw,40,Id(999999));
            }
            if(id==data) {
                if(kind=="short-data")raw.pop_back();
                if(kind=="long-data")raw.push_back(0);
                if(kind=="range")put(raw,8,UINT32_MAX);
            }
            changed.add(id,e.category,e.type,raw);
        }
        QTemporaryDir temp;const auto path=temp.filePath("invalid.gpa_frame");changed.save(path);
        const auto report=validateFrame(path.toStdWString());QVERIFY(report["errors"].get<unsigned>()>0);
        bool located=false;for(const auto &finding:report["findings"])
            if(finding["kind"]=="map_write_rejected"&&finding["event_id"]==event&&finding["resource_id"]==resource)located=true;
        QVERIFY(located);
        Frame frame(path.toStdWString());ReplayOptions options;options.warp=true;Replay replay(frame,options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,replay.run());
        QVERIFY(!replay.counts.contains("Map"));QVERIFY(!replay.counts.contains("map_write_readiness_waits"));
    }
    void failedInvalidCombinationIsNotRetried() {
        Capture capture;capture.add(1,5,0x127,Raw(24));
        Raw command;
        append(command,Id(0));append(command,Id(1));append(command,int32_t(0x80070057));
        append(command,Id(999));append(command,0u);append(command,4u);append(command,0x100000u);append(command,Id(0));
        capture.add(10,7,0x246,command);
        QTemporaryDir temp;const auto path=temp.filePath("failed.gpa_frame");capture.save(path);
        Frame frame(path.toStdWString());QCOMPARE(validateFrame(frame.path())["errors"],nlohmann::json(0));
        ReplayOptions options;options.warp=true;Replay replay(frame,options);replay.run();
        QVERIFY(!replay.counts.contains("Map"));QVERIFY(!replay.counts.contains("map_write_readiness_waits"));
    }
};
QTEST_GUILESS_MAIN(MapNowaitTests)
#include "MapNowaitTests.moc"
