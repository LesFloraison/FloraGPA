#include "ApiExportCapture.h"
#include "app/QueryInspection.h"
#include "application/ApiCommands.h"
#include "application/QueryHistory.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
using Raw = std::vector<uint8_t>;
Raw result(Id query=50, int hr=0, uint32_t size=4) { return apiExportPack(Id(0),Id(1),hr,query,uint8_t(1),1u,size,0u); }
Raw descriptor(Id query=50,uint32_t kind=5,uint32_t flags=0) { return apiExportPack(Id(0),query,uint8_t(1),kind,flags); }
Raw dataSize(Id query=50,uint32_t bytes=4) { return apiExportPack(Id(0),query,bytes); }
void predicate(Capture &c) { c.add(50,5,0x96,apiExportPack(Id(0),Id(1),5u,0u)); }
Json read(const QString &path) { QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read test oracle");return Json::parse(file.readAll().toStdString()); }
Json field(const Json &row,const char *name) { for(const auto &f:row["fields"])if(f["name"]==name)return f["value"];return nullptr; }
Json history(const QueryInspection &prepared,Id event) { const auto row=prepared.find(event);if(!row)throw std::runtime_error("Missing prepared Query");return row->at("query_result"); }
bool evidence(const Json &q,Id event,const char *source) { for(const auto &m:q["metadata"])if(m["event"]==event&&m["source"]==source)return true;return false; }
void equivalent(const std::shared_ptr<const Frame> &frame,const QueryInspection &prepared) {
    size_t count=0;for(const auto &row:inspectCommands(*frame))if(row.contains("query_result")){QVERIFY(prepared.find(row["id"]));QCOMPARE(*prepared.find(row["id"]),row);++count;}
    QCOMPARE(prepared.size(),count);
}
}
class PredicateQueryTests final : public QObject {
    Q_OBJECT
  private slots:
    void ordering_data() {
        QTest::addColumn<bool>("saved");QTest::addColumn<bool>("mixed");
        for(bool saved:{false,true})for(bool mixed:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(saved).arg(mixed)))<<saved<<mixed;
    }
    void ordering() {
        QFETCH(bool,saved);QFETCH(bool,mixed);QTemporaryDir root;Capture c;if(saved)predicate(c);
        c.add(100,7,0x34fb,result());
        c.add(101,7,mixed?0x3152:0x316a,descriptor());c.add(102,7,0x34fb,result());
        c.add(103,7,0x3169,dataSize());c.add(104,7,0x34fb,result());
        c.add(105,7,mixed?0x3151:0x3169,dataSize(50,8));c.add(106,7,0x34fb,result());
        c.add(107,7,0x316a,descriptor(50,7));c.add(108,7,0x34fb,result());
        const auto path=root.filePath("ordering.gpa_frame");c.save(path);auto frame=std::make_shared<Frame>(path.toStdWString());
        const auto prepared=QueryInspection::prepare(frame);equivalent(frame,*prepared);
        QCOMPARE(history(*prepared,100)["status"],Json(saved?"complete":"unknown_type"));
        QCOMPARE(history(*prepared,102)["status"],Json("complete"));QVERIFY(evidence(history(*prepared,102),101,"same_id_GetDesc"));
        QVERIFY(!evidence(history(*prepared,102),103,"same_id_GetDataSize"));
        auto q=history(*prepared,104);QCOMPARE(q["status"],Json("complete"));QCOMPARE(q["fields"][0]["value"],Json(true));
        QVERIFY(evidence(q,101,"same_id_GetDesc"));QVERIFY(evidence(q,103,"same_id_GetDataSize"));
        for(Id event:{106,108}) {q=history(*prepared,event);QCOMPARE(q["status"],Json("metadata_conflict"));QVERIFY(!q["typed_result_complete"].get<bool>());QVERIFY(q["fields"].empty());QCOMPARE(q["captured_hex"],Json("01000000"));}
        for(Id event:{100,102,104,106,108}) {
            const auto observed=history(*prepared,event);
            for(const auto &m:observed["metadata"])if(!m["event"].is_null())QVERIFY(m["event"].get<Id>()<event);
        }
    }
    void boundary_data() {
        QTest::addColumn<QString>("mode");
        for(const auto mode:{"wrong-owner","null-owner","wrong-kind","wrong-size","flags","not-ready","failed","status-only","missing-descriptor","same-description","wrong-resource"})QTest::newRow(mode)<<QString(mode);
    }
    void boundary() {
        QFETCH(QString,mode);QTemporaryDir root;Capture c;
        if(mode=="wrong-resource")c.buffer(50,51,8,0,{0,0,0,0});
        else if(mode!="wrong-owner"&&mode!="null-owner"&&mode!="missing-descriptor")predicate(c);
        const Id owner=mode=="wrong-owner"?60:mode=="null-owner"?0:50;
        const bool badKind=mode=="wrong-kind"||mode=="not-ready"||mode=="failed"||mode=="status-only";
        c.add(100,7,0x316a,mode=="missing-descriptor"?apiExportPack(Id(0),owner,uint8_t(0)):descriptor(owner,badKind?7:5,mode=="flags"?2:0));
        c.add(101,7,0x3169,dataSize(owner,mode=="wrong-size"?8:4));
        c.add(102,7,0x34fb,result(50,mode=="not-ready"?1:mode=="failed"?-1:0,mode=="status-only"?0:4));
        const auto path=root.filePath("boundary.gpa_frame");c.save(path);auto frame=std::make_shared<Frame>(path.toStdWString());
        const auto prepared=QueryInspection::prepare(frame);equivalent(frame,*prepared);const auto q=history(*prepared,102);
        const char *status=mode=="wrong-owner"||mode=="null-owner"||mode=="missing-descriptor"?"unknown_type":mode=="same-description"?"complete":mode=="not-ready"?"not_ready":mode=="failed"?"failed":mode=="status-only"?"status_only":"metadata_conflict";
        QCOMPARE(q["status"],Json(status));QCOMPARE(q["captured_hex"],Json("01000000"));
        if(mode=="not-ready"||mode=="failed"||mode=="status-only")QVERIFY(!q["issues"].empty());
        if(mode=="wrong-owner"||mode=="null-owner")QVERIFY(q["metadata"].empty());
    }
    void malformed_data() { QTest::addColumn<int>("type");QTest::newRow("descriptor")<<0x316a;QTest::newRow("size")<<0x3169; }
    void malformed() {
        QFETCH(int,type);const auto valid=type==0x316a?descriptor():dataSize();QTemporaryDir root;
        for(size_t length=0;length<valid.size()+2;++length) {
            if(length==valid.size())continue;
            Capture c;predicate(c);Raw raw(valid.begin(),valid.begin()+std::min(length,valid.size()));if(length>valid.size())raw.push_back(0xff);
            c.add(100,7,0x34fb,result());c.add(101,7,uint16_t(type),raw);c.add(102,7,0x34fb,result());
            const auto path=root.filePath(QString::number(length)+".gpa_frame");c.save(path);auto frame=std::make_shared<Frame>(path.toStdWString());
            const auto prepared=QueryInspection::prepare(frame);equivalent(frame,*prepared);
            QVERIFY(inspectCommand(*frame,101)["status"]!="decoded");
            QCOMPARE(history(*prepared,100)["status"],Json("complete"));
            QCOMPARE(history(*prepared,102)["status"],Json(length>=16?"metadata_conflict":"complete"));
            if(length>=16)QVERIFY(history(*prepared,102)["issues"].dump().find("101")!=std::string::npos);
        }
        if(type==0x316a) {
            Capture c;predicate(c);auto raw=valid;raw[16]=2;c.add(101,7,0x316a,raw);c.add(102,7,0x34fb,result());
            const auto path=root.filePath("flag.gpa_frame");c.save(path);auto frame=std::make_shared<Frame>(path.toStdWString());
            QCOMPARE(history(*QueryInspection::prepare(frame),102)["status"],Json("metadata_conflict"));
        }
    }
    void original_data() { QTest::addColumn<int>("mode");for(int mode=0;mode<10;++mode)QTest::newRow(qPrintable(QString::number(mode)))<<mode; }
    void original() {
        QFETCH(int,mode);const auto root=qEnvironmentVariable("FLORA_PREDICATE_CREATION_CAPTURES");if(root.isEmpty())QSKIP("Set FLORA_PREDICATE_CREATION_CAPTURES for original predicate metadata");
        const auto directory=root+'/'+QString::number(mode);auto frame=std::make_shared<Frame>((directory+"/capture.gpa_frame").toStdWString());
        const auto rows=inspectCommands(*frame);const auto prepared=QueryInspection::prepare(frame);equivalent(frame,*prepared);
        const auto native=read(directory+"/native/oracle.json"),captured=read(directory+"/captured/oracle.json");
        QVERIFY(native["completed"]==true&&captured["completed"]==true);QCOMPARE(native["frames"],captured["frames"]);
        const bool expected=mode!=0&&mode!=3&&mode!=8&&mode!=9;
        for(const auto &f:native["frames"]){QCOMPARE(f["query_value"],Json(expected));QCOMPARE(f["query_read"],Json(mode!=4));}
        std::map<Id,Id> descriptors,sizes;size_t complete=0,results=0;
        for(const auto &row:rows) {
            const auto id=row["id"].get<Id>();
            if(row["type"]==0x316a){descriptors[field(row,"object").get<Id>()]=id;QCOMPARE(field(row,"query_type"),Json(mode==8?7:5));}
            if(row["type"]==0x3169){sizes[field(row,"object").get<Id>()]=id;QCOMPARE(field(row,"return_data_size"),Json(4));}
            if(!row.contains("query_result"))continue;
            ++results;const auto &q=row["query_result"];const auto object=q["object"].get<Id>();
            QVERIFY(descriptors.contains(object)&&sizes.contains(object));
            QVERIFY(evidence(q,descriptors.at(object),"same_id_GetDesc"));QVERIFY(evidence(q,sizes.at(object),"same_id_GetDataSize"));QVERIFY(q["issues"].empty());
            if(q["status"]=="complete"){++complete;QCOMPARE(q["fields"][0]["value"],Json(expected));}
            else QCOMPARE(q["status"],Json("not_ready"));
        }
        QVERIFY(!descriptors.empty()&&!sizes.empty());if(mode==4)QCOMPARE(results,size_t(0));else QVERIFY(complete>0);
        QTemporaryDir output;exportCommands(*frame,output.path().toStdWString(),"GetData");
        const auto exported=read(output.filePath("commands.json"));
        for(const auto &row:exported["commands"])if(row.contains("query_result"))QCOMPARE(*prepared->find(row["id"]),row);
    }
};
QTEST_GUILESS_MAIN(PredicateQueryTests)
#include "PredicateQueryTests.moc"
