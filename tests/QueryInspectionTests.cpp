#include "ApiExportCapture.h"
#include "app/Models.h"
#include "application/QueryHistory.h"
#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
using Raw = std::vector<uint8_t>;
Raw wire(uint16_t type) {
    if (type == 0x3151 || type == 0x3169) return apiExportPack(Id(0), Id(50), 4u);
    if (type == 0x3152 || type == 0x316a) return apiExportPack(Id(0), Id(50), uint8_t(1), 0u, 0u);
    if (type == 0x3074 || type == 0x3235 || type == 0x33a8 || type == 0x3471 || type == 0x34b2 || type == 0x358d)
        return apiExportPack(Id(0), Id(1), 0, uint8_t(1), 0u, 0u, Id(50));
    return apiExportPack(Id(0), Id(1), 0, Id(50), uint8_t(1), 1u, 4u, 0u);
}
void equivalent(const std::shared_ptr<const Frame> &frame) {
    const auto prepared = QueryInspection::prepare(frame); size_t count = 0;
    for (const auto &row : inspectCommands(*frame)) {
        const auto cached = prepared->find(row["id"].get<Id>());
        if (row.contains("query_result")) { QVERIFY(cached); QCOMPARE(*cached, row); ++count; }
        else QVERIFY(!cached);
    }
    QCOMPARE(prepared->size(), count);
}
}
class QueryInspectionTests final : public QObject {
    Q_OBJECT
  private slots:
    void families_data() {
        QTest::addColumn<int>("create"); QTest::addColumn<int>("get");
        for (int create : {0x3074,0x3235,0x33a8,0x3471,0x34b2,0x358d})
            for (int get : {0x30b4,0x31b4,0x331d,0x33e3,0x34fb})
                QTest::newRow(qPrintable(QString("%1-%2").arg(create,0,16).arg(get,0,16))) << create << get;
    }
    void families() {
        QFETCH(int, create); QFETCH(int, get); QTemporaryDir root; Capture c;
        c.add(100,7,uint16_t(get),wire(uint16_t(get)));
        c.add(101,7,uint16_t(create),wire(uint16_t(create)));
        c.add(102,7,0x3151,wire(0x3151));
        c.add(103,7,uint16_t(get),wire(uint16_t(get)));
        c.add(104,7,0x3152,apiExportPack(Id(0),Id(50),uint8_t(1),2u,0u));
        c.add(105,7,uint16_t(get),wire(uint16_t(get)));
        const auto path=root.filePath("families.gpa_frame"); c.save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString()); equivalent(frame);
        const auto prepared=QueryInspection::prepare(frame);
        QCOMPARE(prepared->size(),size_t(3));
        QCOMPARE(prepared->find(100)->at("query_result")["status"],Json("unknown_type"));
        QCOMPARE(prepared->find(103)->at("query_result")["status"],Json("complete"));
        QCOMPARE(prepared->find(105)->at("query_result")["status"],Json("metadata_conflict"));
    }
    void malformed_data() {
        QTest::addColumn<int>("type");
        for (int type : {0x3151,0x3152,0x3169,0x316a,0x3074,0x3235,0x33a8,0x3471,0x34b2,0x358d,0x30b4,0x31b4,0x331d,0x33e3,0x34fb})
            QTest::newRow(qPrintable(QString::number(type,16))) << type;
    }
    void malformed() {
        QFETCH(int,type); QTemporaryDir root;
        for (int variant=0;variant<5;++variant) {
            Capture c; auto raw=wire(uint16_t(type));
            if (variant==0) raw.clear();
            if (variant==1) raw.resize(15);
            if (variant==2) raw.resize(17);
            if (variant==3) raw.pop_back();
            if (variant==4) raw.push_back(0xff);
            c.add(100,7,uint16_t(type),raw); c.add(101,7,0x34fb,wire(0x34fb));
            const auto path=root.filePath(QString::number(variant)+".gpa_frame"); c.save(path);
            auto frame=std::make_shared<Frame>(path.toStdWString()); equivalent(frame);
        }
    }
    void predicates_data() {
        QTest::addColumn<int>("mode");
        for (int mode=0;mode<4;++mode) QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void predicates() {
        QFETCH(int,mode); QTemporaryDir root; Capture c;
        if(mode==3) c.buffer(50,51,8,0,{0,0,0,0});
        else c.add(50,5,0x96,apiExportPack(Id(0),Id(1),mode==1?7u:5u,mode==2?2u:0u));
        c.add(100,7,0x34fb,wire(0x34fb)); const auto path=root.filePath("predicate.gpa_frame");c.save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString());equivalent(frame);
        QCOMPARE(QueryInspection::prepare(frame)->find(100)->at("query_result")["status"],Json(mode<2?"complete":"metadata_conflict"));
    }
    void ownership() {
        QTemporaryDir root;const auto path=root.filePath("capture.gpa_frame");apiExportCapture().save(path);
        auto a=std::make_shared<Frame>(path.toStdWString()), b=std::make_shared<Frame>(path.toStdWString());
        auto queries=QueryInspection::prepare(a); CaptureModel model(CaptureModel::Kind::Commands);
        QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,model.setFrame(a));
        model.setFrame(a,queries); const auto *first=&model.command(102);const auto expected=*first;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,model.setFrame(b,queries));
        QCOMPARE(&model.command(102),first);QCOMPARE(model.command(102),expected);
        std::weak_ptr<const Frame> lifetime=a; a.reset();queries.reset();QVERIFY(!lifetime.expired());
        QCOMPARE(model.command(102),expected);model.setFrame(nullptr);QVERIFY(lifetime.expired());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,model.command(102));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,QueryInspection::prepare(nullptr));
        model.setFrame(b,QueryInspection::prepare(b));QCOMPARE(model.command(102),expected);
    }
    void cancellation_data() { QTest::addColumn<QString>("mode");for(const auto name:{"before","records","before-publish"})QTest::newRow(name)<<QString(name); }
    void cancellation() {
        QFETCH(QString,mode);QTemporaryDir root;const auto path=root.filePath("capture.gpa_frame");apiExportCapture(10000).save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString());size_t calls=0,total=0;
        const auto complete=QueryInspection::prepare(frame,[&]{++total;return false;});
        const auto at=mode=="before"?size_t(1):mode=="records"?size_t(12):total;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,QueryInspection::prepare(frame,[&]{return ++calls==at;}));
        QCOMPARE(calls,at);const auto retry=QueryInspection::prepare(frame);QCOMPARE(retry->size(),complete->size());
        QCOMPARE(*retry->find(102),*complete->find(102));
    }
    void original_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("gf2")<<QString("GF2_Exilium_2026_03_03__00_19_35.gpa_frame");
        QTest::newRow("bf1")<<QString("bf1_2026_01_21__16_53_05.gpa_frame");
        QTest::newRow("helldivers")<<QString("helldivers2_2026_04_02__18_02_58.gpa_frame");
    }
    void original() {
        QFETCH(QString,name);const auto root=qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if(root.isEmpty())QSKIP("Set FLORA_TEST_CAPTURE_DIR for original Query inspection");
        auto frame=std::make_shared<Frame>((root+'/'+name).toStdWString());equivalent(frame);
        QElapsedTimer timer;timer.start();const auto prepared=QueryInspection::prepare(frame);const auto preparation=timer.nsecsElapsed();
        CaptureModel model(CaptureModel::Kind::Commands);model.setFrame(frame,prepared);timer.restart();size_t count=0;
        for(const auto &[id,e]:frame->entries())if(prepared->find(id)){QCOMPARE(&model.command(id),prepared->find(id));++count;}
        qInfo()<<name<<"Query rows"<<count<<"preparation ns"<<preparation<<"lookup ns"<<timer.nsecsElapsed();
        QVERIFY(count>0);
    }
};
QTEST_GUILESS_MAIN(QueryInspectionTests)
#include "QueryInspectionTests.moc"
