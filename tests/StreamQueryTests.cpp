#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "application/PredicateInspector.h"
#include "application/QueryHistory.h"
#include "application/SetterEdits.h"
#include "core/QueryCompletion.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
std::vector<uint8_t> bytes(const QString &path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read stream oracle");
    auto b=f.readAll();return {b.begin(),b.end()};
}
std::filesystem::path capture(int mode) {
    const auto root=qEnvironmentVariable("FLORA_STREAM_QUERY_CAPTURES");
    if(root.isEmpty())throw std::runtime_error("Set FLORA_STREAM_QUERY_CAPTURES to original stream corpus");
    return (root+'/'+QString::number(mode)+"/capture.gpa_frame").toStdWString();
}
}
class StreamQueryTests final:public QObject {
    Q_OBJECT
  private slots:
    void originals_data(){
        QTest::addColumn<int>("mode");QTest::addColumn<bool>("warp");
        for(int mode=0;mode<48;++mode)for(bool warp:{false,true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp?"warp":"hardware")))<<mode<<warp;
    }
    void originals(){
        if(qEnvironmentVariableIsEmpty("FLORA_STREAM_QUERY_CAPTURES"))QSKIP("Set original stream Query corpus");
        QFETCH(int,mode);QFETCH(bool,warp);Frame frame(capture(mode));
        const auto folder=QString::fromStdWString(frame.path().parent_path().wstring())+"/hardware/";
        QFile inventory(qEnvironmentVariable("FLORA_STREAM_QUERY_CAPTURES")+"/manifest.json");QVERIFY(inventory.open(QIODevice::ReadOnly));
        const auto manifest=nlohmann::json::parse(inventory.readAll().toStdString());
        bool hashVerified=false;for(const auto &row:manifest["cases"])if(row["id"]=="stream_predicate_"+std::to_string(mode)){
            QCOMPARE(frame.sha256(),row["sha256"].get<std::string>());hashVerified=true;
        }QVERIFY(hashVerified);
        const auto expected=bytes(folder+"expected.rgba");
        Id query=0,predicate=0,soDraw=0,queryEnd=0;std::array<Id,4> buffers{};
        for(const auto &[id,e]:frame.entries()){
            if(e.category==5&&e.type==0x96){const auto d=readPredicate(frame,id);if(isStreamOverflowQuery(d.type)){query=id;QCOMPARE(d.type,uint32_t(9+2*(mode%4)));}else predicate=id;}
            if(e.category==7&&e.type==0x37){const auto s=frame.state(frame.event(id).state);if(s.stages[3].shader){soDraw=id;buffers=s.so;}}
        }
        QVERIFY(query&&predicate&&soDraw);for(auto id:buffers)QVERIFY(id);
        for(const auto &[id,e]:frame.entries())if(e.category==7&&predicateOperation(e.type)==PredicateOperation::End)
            if(readPredicateCommand(e.type,frame.payload(id)).resource==query)queryEnd=id;
        QVERIFY(queryEnd);
        const auto validation=validateFrame(frame.path());QCOMPARE(validation["errors"],nlohmann::json(0));
        const auto completions=auditQueryCompletions(frame);bool complete=false;
        for(const auto &[id,q]:completions)if(q.resource==query&&q.result==0){QVERIFY(q.error.empty());QVERIFY(q.missing.empty());QCOMPARE(q.end,queryEnd);complete=true;}
        QVERIFY(complete);
        const auto description=describePredicate(frame,query);
        QCOMPARE(description["bindable_predicate"],nlohmann::json(false));
        QCOMPARE(description["initial_result_source"],nlohmann::json("unissued_saved_stream_query"));
        ReplayOptions options;options.warp=warp;Replay replay(frame,options);
        for(int repeat=0;repeat<2;++repeat){
            replay.run();QCOMPARE(replay.output(replay.lastOutputResource()).rgba,expected);
            QCOMPARE(replay.readPredicateResult(query).value,std::optional<bool>((mode/4)%3==1));
            QCOMPARE(replay.readPredicateResult(predicate).value,std::optional<bool>((mode/4)%3!=0));
            for(unsigned s=0;s<4;++s)QCOMPARE(replay.readBuffer(buffers[s]),bytes(folder+QString("expected-stream%1.bin").arg(s)));
        }
        Id image=0;for(const auto &[id,e]:frame.entries())if(e.category==5&&e.type==0x85){
            const auto resource=frame.resource(id);if(resource.desc[8]&D3D11_BIND_RENDER_TARGET)image=id;
        }QVERIFY(image);
        // Helper rendering must not contaminate an active stream query. A TRUE
        // first segment followed by an empty FALSE segment must remain TRUE.
        bool isolated=false;
        replay.run({},[&](Id id,bool after,auto *,const auto &){if(id==soDraw&&after){replay.previewTexture(image);isolated=true;}});
        QVERIFY(isolated);QCOMPARE(replay.output(replay.lastOutputResource()).rgba,expected);
        QCOMPARE(replay.readPredicateResult(query).value,std::optional<bool>((mode/4)%3==1));
        for(unsigned s=0;s<4;++s)QCOMPARE(replay.readBuffer(buffers[s]),bytes(folder+QString("expected-stream%1.bin").arg(s)));
        options.disabled.insert(soDraw);Replay disabled(frame,options);disabled.run();
        QCOMPARE(disabled.readPredicateResult(query).value,std::optional<bool>(false));
        QCOMPARE(disabled.readPredicateResult(predicate).value,std::optional<bool>(false));
        std::vector<uint8_t> noWork;
        for(unsigned i=0;i<64;++i){const bool green=(mode/12)%2;noWork.insert(noWork.end(),{uint8_t(green?0:255),uint8_t(green?255:0),uint8_t(green?0:255),255});}
        QCOMPARE(disabled.output(disabled.lastOutputResource()).rgba,noWork);
    }
    void malformedAndBinding_data(){QTest::addColumn<QString>("kind");for(auto k:{"truncated","trailing","flags","statistics","binding","creation"})QTest::newRow(k)<<QString(k);}
    void absentCompletionRemainsUnavailable(){
        if(qEnvironmentVariableIsEmpty("FLORA_STREAM_QUERY_CAPTURES"))QSKIP("Set original stream Query corpus");
        Frame frame(capture(4));Id query=0,begin=0,end=0;
        for(const auto &[id,e]:frame.entries())if(e.category==5&&e.type==0x96&&isStreamOverflowQuery(readPredicate(frame,id).type))query=id;
        for(const auto &[id,e]:frame.entries())if(e.category==7&&predicateOperation(e.type)){
            const auto call=readPredicateCommand(e.type,frame.payload(id));if(call.resource!=query)continue;
            if(call.operation==PredicateOperation::Begin)begin=id;if(call.operation==PredicateOperation::End)end=id;
        }
        QVERIFY(query&&begin&&end);ReplayOptions options;options.warp=true;options.until=begin;options.before=true;
        Replay before(frame,options);before.run();const auto unavailable=before.readPredicateResult(query);
        QCOMPARE(unavailable.status,std::string("unissued"));QVERIFY(!unavailable.value);
        options.until=0;options.before=false;options.disabled.insert(end);Replay disabled(frame,options);
        bool located=false;try{disabled.run();}catch(const std::exception &e){located=std::string(e.what()).find("GetData completion requires its captured predicate interval")!=std::string::npos;}
        QVERIFY(located);
        Capture incomplete;
        for(const auto &[id,e]:frame.entries())if(id!=begin&&id!=end){const auto p=frame.payload(id);incomplete.add(id,e.category,e.type,{p.begin(),p.end()});}
        QTemporaryDir temp;const auto path=temp.filePath("missing-interval.gpa_frame");incomplete.save(path);Frame missing(path.toStdWString());
        bool notice=false;for(const auto &[id,q]:auditQueryCompletions(missing))if(q.resource==query&&q.result==0){QVERIFY(q.error.empty());QVERIFY(!q.missing.empty());QCOMPARE(q.end,Id(0));notice=true;}
        QVERIFY(notice);ReplayOptions ordinary;ordinary.warp=true;Replay replay(missing,ordinary);replay.run();
        const auto value=replay.readPredicateResult(query);QCOMPARE(value.status,std::string("unissued"));QVERIFY(!value.value);
    }
    void malformedAndBinding(){
        if(qEnvironmentVariableIsEmpty("FLORA_STREAM_QUERY_CAPTURES"))QSKIP("Set original stream Query corpus");
        QFETCH(QString,kind);Frame original(capture(4));Id query=0,bind=0,device=0;
        for(const auto &[id,e]:original.entries()){
            if(e.category==5&&e.type==0x96){const auto d=readPredicate(original,id);if(isStreamOverflowQuery(d.type)){query=id;device=d.device;}}
            if(e.category==7&&predicateOperation(e.type)==PredicateOperation::Set&&readPredicateCommand(e.type,original.payload(id)).resource)bind=id;
        }
        QVERIFY(query&&bind);Capture changed;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,validatePredicateSetter(original,bind,{{"predicate",query},{"predicate_value",0}}));
        for(const auto &[id,e]:original.entries()){
            const auto p=original.payload(id);std::vector<uint8_t> raw(p.begin(),p.end());
            if(id==query){
                if(kind=="truncated")raw.pop_back();if(kind=="trailing")raw.push_back(0);
                if(kind=="flags")put(raw,20,1u);if(kind=="statistics")put(raw,16,8u);
            }
            if(id==bind&&kind=="binding")put(raw,16,query);
            changed.add(id,e.category,e.type,raw);
        }
        if(kind=="creation"){
            std::vector<uint8_t> raw;append(raw,Id(0));append(raw,device);append(raw,int32_t(0));append(raw,uint8_t(1));
            append(raw,9u);append(raw,0u);append(raw,Id(999999));changed.add(999998,7,0x358e,raw);
        }
        QTemporaryDir temp;const auto path=temp.filePath("invalid.gpa_frame");changed.save(path);Frame frame(path.toStdWString());
        QVERIFY(validateFrame(frame.path())["errors"].get<unsigned>()>0);
        ReplayOptions options;options.warp=true;Replay replay(frame,options);QVERIFY_THROWS_EXCEPTION(std::runtime_error,replay.run());
    }
};
QTEST_GUILESS_MAIN(StreamQueryTests)
#include "StreamQueryTests.moc"
