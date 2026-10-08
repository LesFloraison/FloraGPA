#include "ApiExportCapture.h"
#include "app/Models.h"
#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
QStringList words(const QString &text) { return text.toCaseFolded().split(QRegularExpression("\\s+"), Qt::SkipEmptyParts); }
void equivalent(const std::shared_ptr<const Frame> &frame) {
    const auto queries=QueryInspection::prepare(frame);
    QElapsedTimer timer;timer.start();const auto index=CommandSearchIndex::prepare(frame,queries);
    const auto preparation=timer.elapsed();size_t count=0,checks=0;
    for(const auto &[id,entry]:frame->entries()) {
        if(entry.category!=7)continue;
        const auto row=queries->find(id)?*queries->find(id):inspectCommand(*frame,id);
        const QStringList terms{"", " \t\n", "GeTdAtA", "copy resource", "decoded", "partial", "invalid", "unknown", ".*", "é", "\u2003decoded\u2003", QString::number(id), QString::fromStdString(row["name"].get<std::string>())};
        std::vector<std::optional<Id>> resources{std::nullopt,Id(0),std::numeric_limits<Id>::max()};
        for(const auto &reference:row["references"]) {
            resources.push_back(reference["id"].get<Id>());
            if(reference.contains("resource"))resources.push_back(reference["resource"].get<Id>());
        }
        for(const auto &text:terms)for(const auto resource:resources) {
            QCOMPARE(index->matches(id,words(text),resource),commandMatches(row,text.toStdString(),resource));++checks;
        }
        ++count;
    }
    QCOMPARE(index->size(),count);QVERIFY(!index->matches(0,{},{}));
    qInfo()<<"API search records"<<count<<"matching comparisons"<<checks<<"preparation ms"<<preparation;
}
}
class CommandSearchTests final:public QObject {
    Q_OBJECT
  private slots:
    void mixedRecords() {
        QTemporaryDir root;auto capture=apiExportCapture();capture.uav(10,2,0);
        capture.add(108,7,0x3e,apiExportPack(Id(0),Id(1),Id(10),Id(999)));
        capture.add(109,7,0x244,apiExportPack(Id(0),Id(1),uint8_t(7)));
        capture.add(std::numeric_limits<Id>::max(),7,0xffee,apiExportPack(Id(0),Id(1)));
        const auto path=root.filePath("mixed.gpa_frame");capture.save(path);
        const auto frame=std::make_shared<Frame>(path.toStdWString());equivalent(frame);
        const auto queries=QueryInspection::prepare(frame);
        CaptureModel model(CaptureModel::Kind::Commands);
        model.setFrame(frame,queries,CommandSearchIndex::prepare(frame,queries));
        CaptureFilter filter;filter.setSourceModel(&model);filter.setFilterKeyColumn(-1);
        for(const auto &text:QStringList{"", "GeTdAtA", "copy resource", "invalid", " \t ", "decoded", ""}) {
            filter.setFilterFixedString(text);
            for(const auto resource:std::vector<std::optional<Id>>{std::nullopt,Id(1),Id(2),Id(10),Id(999)}) {
                filter.referencedResource=resource;filter.refresh();std::vector<Id> expected,actual;
                for(const auto &[id,entry]:frame->entries())if(entry.category==7) {
                    const auto row=queries->find(id)?*queries->find(id):inspectCommand(*frame,id);
                    if(commandMatches(row,text.toStdString(),resource))expected.push_back(id);
                }
                for(int row=0;row<filter.rowCount();++row)actual.push_back(filter.index(row,0).data(Qt::UserRole).toULongLong());
                QCOMPARE(actual,expected);QCOMPARE(model.cachedCommandCount(),size_t(0));
            }
        }
    }
    void cacheAndLifetime() {
        QTemporaryDir root;const auto path=root.filePath("many.gpa_frame");apiExportCapture(10000).save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString());auto queries=QueryInspection::prepare(frame);
        auto search=CommandSearchIndex::prepare(frame,queries);
        CaptureModel model(CaptureModel::Kind::Commands);model.setFrame(frame,queries,search);
        QAbstractItemModelTester tester(&model,QAbstractItemModelTester::FailureReportingMode::QtTest);
        CaptureFilter filter;filter.setSourceModel(&model);filter.setFilterKeyColumn(-1);
        filter.setFilterFixedString("decoded");QVERIFY(filter.rowCount()>10000);
        QCOMPARE(model.cachedCommandCount(),size_t(0)); // Searching must not fill the detail cache.
        const auto first=model.command(1000),query=model.command(102);
        for(Id id=1001;id<11000;++id) {
            QCOMPARE(model.command(id),inspectCommand(*frame,id));
            QVERIFY(model.cachedCommandCount()<=CaptureModel::detailCacheLimit);
        }
        QCOMPARE(model.cachedCommandCount(),CaptureModel::detailCacheLimit);
        QCOMPARE(first,inspectCommand(*frame,1000));QCOMPARE(model.command(1000),first);
        QCOMPARE(model.command(102),query);
        auto other=std::make_shared<Frame>(path.toStdWString());auto otherQueries=QueryInspection::prepare(other);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,model.setFrame(other,otherQueries,search));
        QCOMPARE(model.command(1000),first);
        std::weak_ptr<const Frame> lifetime=frame;frame.reset();queries.reset();search.reset();
        QVERIFY(!lifetime.expired());model.setFrame(nullptr);QVERIFY(lifetime.expired());
        QCOMPARE(model.cachedCommandCount(),size_t(0));QCOMPARE(first["id"],nlohmann::json(1000));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,CommandSearchIndex::prepare(nullptr,otherQueries));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,CommandSearchIndex::prepare(other,{}));
    }
    void cancellation_data(){QTest::addColumn<QString>("mode");for(auto mode:{"before","records","references","before-publish"})QTest::newRow(mode)<<QString(mode);}
    void cancellation(){
        QFETCH(QString,mode);QTemporaryDir root;const auto path=root.filePath("cancel.gpa_frame");apiExportCapture(1000).save(path);
        auto frame=std::make_shared<Frame>(path.toStdWString());auto queries=QueryInspection::prepare(frame);size_t total=0,calls=0;
        const auto expected=CommandSearchIndex::prepare(frame,queries,[&]{++total;return false;});
        const size_t at=mode=="before"?1:mode=="records"?20:mode=="references"?6:total;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,CommandSearchIndex::prepare(frame,queries,[&]{return ++calls==at;}));
        QCOMPARE(calls,at);const auto retry=CommandSearchIndex::prepare(frame,queries);QCOMPARE(retry->size(),expected->size());
    }
    void original_data(){
        QTest::addColumn<QString>("name");QTest::addColumn<QString>("hash");
        const auto corpus=qEnvironmentVariable("FLORA_COMMAND_SEARCH_CORPUS");
        if(!corpus.isEmpty()) {
            QFile input(corpus);QVERIFY(input.open(QIODevice::ReadOnly));
            const auto rows=nlohmann::json::parse(input.readAll().toStdString());
            QVERIFY(rows.is_array()&&!rows.empty());
            for(const auto &row:rows) {
                const auto id=row.at("sha256").get<std::string>();
                QTest::newRow(id.c_str())<<QString::fromStdString(row.at("path").get<std::string>())<<QString::fromStdString(id);
            }
            return;
        }
        for(auto name:{"GF2_Exilium_2026_03_03__00_19_35.gpa_frame","bf1_2026_01_21__16_53_05.gpa_frame","helldivers2_2026_04_02__18_02_58.gpa_frame","analysis/capture_samples/legacy/deferred1-each0-restore0-repeat2.gpa_frame","analysis/capture_samples/legacy/deferred1-each1-restore1-repeat2.gpa_frame"})QTest::newRow(name)<<QString(name)<<QString{};
    }
    void original(){
        QFETCH(QString,name);QFETCH(QString,hash);QString path=name;
        if(hash.isEmpty()) {
            const auto root=qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
            if(root.isEmpty())QSKIP("Set FLORA_TEST_CAPTURE_DIR for original API search equivalence");
            path=root+'/'+name;
        }
        auto frame=std::make_shared<Frame>(path.toStdWString());
        if(!hash.isEmpty())QCOMPARE(QString::fromStdString(frame->sha256()),hash);
        equivalent(frame);
    }
};
QTEST_GUILESS_MAIN(CommandSearchTests)
#include "CommandSearchTests.moc"
