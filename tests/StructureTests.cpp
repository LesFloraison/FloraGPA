#include "StructureCapture.h"
#include "app/StructureModel.h"
#include "app/StructureExport.h"
#include "application/ContextInspector.h"
#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Read failed"); return file.readAll(); }
void save(const QString &path, const QByteArray &bytes) { QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) throw std::runtime_error("Write failed"); }
QModelIndex named(QAbstractItemModel &model, const QString &key, const QModelIndex &parent = {}) {
    for (int row = 0; row < model.rowCount(parent); ++row) {
        auto index = model.index(row, 0, parent); if (index.data().toString() == key) return index;
    }
    return {};
}
}
class StructureTests final : public QObject {
    Q_OBJECT
  private slots:
    void model() {
        QTemporaryDir root; const auto path = root.filePath("model.gpa_frame"); testing::structureCapture().save(path);
        auto frame = std::make_shared<Frame>(path.toStdWString());
        auto doc = std::make_shared<const Json>(inspectCommandLists(*frame)); StructureModel model(doc, frame);
        QVERIFY(!named(model,"limits").isValid()); QVERIFY(!named(model,"source").isValid()); QVERIFY(!model.rootToolTip().isEmpty());
        QCOMPARE(named(model,"execution_supported").siblingAtColumn(1).data().toString(),QString("false"));
        auto lists=named(model,"command_lists"); QCOMPARE(model.rowCount(lists),1);
        auto list=model.index(0,0,lists); auto events=named(model,"recorded_events",list);
        QCOMPARE(model.rowCount(events),256); QVERIFY(model.canFetchMore(events));
        QSignalSpy inserted(&model,&QAbstractItemModel::rowsInserted);
        while(model.canFetchMore(events)) model.fetchMore(events);
        QCOMPARE(model.rowCount(events),20000); QVERIFY(!inserted.empty());
        auto event=named(model,"id",model.index(19999,0,events)).siblingAtColumn(1);
        QCOMPARE(event.data(Qt::UserRole).toULongLong(),qulonglong(20999));
        QCOMPARE(model.parent(event),model.index(19999,0,events));
        QVERIFY(model.data(event,Qt::ToolTipRole).toString().contains("Double-click"));
        QCOMPARE(model.rowCount(events.siblingAtColumn(1)),0);
        QVERIFY(!model.index(-1,0).isValid()); QVERIFY(!model.index(20000,0,events).isValid());
        // The view never rewrites the document used for export.
        QCOMPARE(*doc,inspectCommandLists(*frame));
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest); tester.setUseFetchMore(false);
    }
    void values() {
        auto doc=std::make_shared<const Json>(Json{{"value",Json::array({nullptr,true,-5,2.5,"测试\n\"\\"})},{"scope","Hidden scope"}});
        StructureModel model(doc,{}); auto values=named(model,"value");
        QCOMPARE(model.rowCount(values),5); QCOMPARE(model.rootToolTip(),QString("Hidden scope"));
        for(int i=0;i<5;++i) {auto index=model.index(i,1,values);const auto &v=(*doc)["value"][i];QCOMPARE(index.data().toString(),QString::fromStdString(v.is_string()?v.get<std::string>():v.dump()));QVERIFY(!index.data(Qt::UserRole).isValid());}
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument,StructureModel(nullptr,{}));
    }
    void exportFile_data() {
        QTest::addColumn<QString>("mode");
        for(const auto name:{"exact","before","during","publication","invalid-utf8","locked","missing-parent"}) QTest::newRow(name)<<QString(name);
    }
    void exportFile() {
        QFETCH(QString,mode);QTemporaryDir root;const auto path=root.filePath("报告.json");save(path,"Old document");
        Json doc{{"unicode","测试\n\"\\"},{"array",{nullptr,true,1,-7,1.25}},{"nested",{{"empty",Json::array()}}}};
        if(mode=="during")doc["large"]=std::string(8*1024*1024,'x');
        if(mode=="invalid-utf8")doc["invalid"]=std::string(1,char(0xff));
        int checks=0;CancelCheck cancel=[&]{++checks;return mode=="before"||(mode=="during"&&checks>=3)||(mode=="publication"&&checks>=3);};
        HANDLE lock=INVALID_HANDLE_VALUE;
        if(mode=="locked"){lock=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);QVERIFY(lock!=INVALID_HANDLE_VALUE);}
        bool failed=false;
        try{exportStructureFile(mode=="missing-parent"?root.filePath("absent/file.json"):path,doc,cancel);}catch(const std::exception &){failed=true;}
        if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
        QCOMPARE(failed,mode!="exact");
        if(mode=="exact")QCOMPARE(read(path),QByteArray::fromStdString(doc.dump(2)+"\n"));else QCOMPARE(read(path),QByteArray("Old document"));
        if(mode!="invalid-utf8"){exportStructureFile(path,doc);QCOMPARE(read(path),QByteArray::fromStdString(doc.dump(2)+"\n"));}
        QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot).size(),1);
    }
    void cancellation_data(){QTest::addColumn<QString>("part");for(auto name:{"contexts","lists","context-json","list"})QTest::newRow(name)<<QString(name);}
    void cancellation() {
        QFETCH(QString,part);QTemporaryDir root;const auto path=root.filePath("cancel.gpa_frame");testing::structureCapture().save(path);Frame frame(path.toStdWString());
        const auto contexts=inspectContexts(frame),lists=inspectCommandLists(frame);int checks=0;CancelCheck cancel=[&]{return ++checks>=5;};
        if(part=="contexts")QVERIFY_THROWS_EXCEPTION(OperationCancelled,inspectContexts(frame,cancel));
        else if(part=="lists")QVERIFY_THROWS_EXCEPTION(OperationCancelled,inspectCommandLists(frame,cancel));
        else if(part=="list")QVERIFY_THROWS_EXCEPTION(OperationCancelled,inspectCommandList(frame,200,[]{return true;}));
        else {ContextDescription context;context.evidence.resize(20000);QVERIFY_THROWS_EXCEPTION(OperationCancelled,contextJson(context,cancel));}
        QCOMPARE(inspectContexts(frame),contexts);QCOMPARE(inspectCommandLists(frame),lists);
    }
};
QTEST_GUILESS_MAIN(StructureTests)
#include "StructureTests.moc"
