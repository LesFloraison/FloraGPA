#include "app/WorkerReport.h"
#include "application/DrawResources.h"
#include <QBuffer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
void save(const QString &path, const QByteArray &bytes) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray png(int width = 32, int height = 16) {
    QImage image(width, height, QImage::Format_RGBA8888);
    image.fill(QColor(53, 97, 179, 211));
    QByteArray bytes; QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) qFatal("Cannot create PNG fixture");
    return bytes;
}
Json fixture(const QString &directory) {
    DrawResourceBinding binding;
    binding.key="in/PS/SRV/0";binding.kind="SRV";binding.stage="PS";binding.texture=true;
    binding.width=32;binding.height=16;
    binding.image.resource=UINT64_MAX;binding.image.view=UINT64_MAX-1;binding.image.event=UINT64_MAX-2;
    auto row=drawResourceJson(binding);row["preview"]="thumb.png";
    auto alias=row; alias["key"]="in/VS/SRV/0";
    auto failed=row; failed["key"]="out/OM/RTV/0"; failed.erase("preview"); failed["preview_error"]="Unavailable fixture";
    auto unrequested=row; unrequested["key"]="in/PS/CB/0"; unrequested.erase("preview");
    Json report{{"completed",true},{"event",UINT64_MAX-2},{"bindings",{row,alias,failed,unrequested}}};
    save(directory+"/report.json",QByteArray::fromStdString(report.dump()));
    save(directory+"/thumb.png",png());
    return report;
}
}
class ThumbnailOutputTests final : public QObject {
    Q_OBJECT
  private slots:
    void validAndSharedPreview() {
        QTemporaryDir dir; fixture(dir.path());
        auto output=readWorkerOutput(dir.path(),"draw-resources");
        QVERIFY(output.thumbnails);
        QVERIFY(output.report.isEmpty() && output.replayReport.is_null());
        const auto &result=*output.thumbnails;
        QCOMPARE(result.event,UINT64_MAX-2); QCOMPARE(result.rows.size(),size_t(4));
        const auto &row=result.rows.at("in/PS/SRV/0");
        QCOMPARE(row.resource,UINT64_MAX); QCOMPARE(row.view,UINT64_MAX-1);
        QCOMPARE(row.display.size(),QSize(32,16));
        QCOMPARE(row.display.format(),QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(row.display,QImage::fromData(png()).convertToFormat(QImage::Format_ARGB32_Premultiplied));
        QCOMPARE(row.display,result.rows.at("in/VS/SRV/0").display);
        QVERIFY(!row.error);
        QCOMPARE(*result.rows.at("out/OM/RTV/0").error,QString("Unavailable fixture"));
        QVERIFY(result.rows.at("in/PS/CB/0").display.isNull());
    }
    void malformedEnvelopeAndIdentity() {
        QTemporaryDir dir; const auto report=fixture(dir.path());
        auto check=[&](const Json &bad) {
            save(dir.filePath("report.json"),QByteArray::fromStdString(bad.dump()));
            QVERIFY_THROWS_EXCEPTION(std::exception,readThumbnailOutput(dir.path()));
        };
        check(Json::array());
        for (auto value : {Json(false),Json("true"),Json(1),Json(nullptr)}) {
            auto bad=report;bad["completed"]=value;check(bad);
        }
        { auto bad=report;bad["bindings"]=Json::object();check(bad); }
        { auto bad=report;bad["bindings"][1]["key"]=bad["bindings"][0]["key"];check(bad); }
        { auto bad=report;bad["bindings"][0]["key"]="";check(bad); }
        { auto bad=report;bad["bindings"][0]["event"]=17;check(bad); }
        { auto bad=report;bad["bindings"][0]["preview_error"]="ambiguous";check(bad); }
        for (const auto field : {"resource","view","event","format","mip","layer","slice","sample","slot"})
            for (const auto value : {Json(-1),Json(1.5),Json("1"),Json(true),Json(nullptr)}) {
                if (std::string(field)=="sample" && value.is_null()) continue;
                auto bad=report;bad["bindings"][0][field]=value;check(bad);
            }
        const auto good=QByteArray::fromStdString(report.dump());
        for (qsizetype i=0;i<good.size();++i) {
            save(dir.filePath("report.json"),good.left(i));
            QVERIFY_THROWS_EXCEPTION(std::exception,readThumbnailOutput(dir.path()));
        }
        fixture(dir.path());
        QCOMPARE(readThumbnailOutput(dir.path()).rows.size(),size_t(4));
    }
    void pathsAndPngBoundaries() {
        QTemporaryDir dir; const auto report=fixture(dir.path());
        for (const auto name : {"../outside.png","..\\outside.png","C:thumb.png","/thumb.png","thumb.PNG",""}) {
            auto bad=report;bad["bindings"][0]["preview"]=name;
            save(dir.filePath("report.json"),QByteArray::fromStdString(bad.dump()));
            QVERIFY_THROWS_EXCEPTION(std::exception,readThumbnailOutput(dir.path()));
        }
        fixture(dir.path());
        QVERIFY(QFile::remove(dir.filePath("thumb.png")));
        QVERIFY_THROWS_EXCEPTION(std::exception,readThumbnailOutput(dir.path()));
        for (const auto &bad : {QByteArray(),QByteArray("not a PNG"),png().left(png().size()/2),png(97,16),png(32,97)}) {
            save(dir.filePath("thumb.png"),bad);
            QVERIFY_THROWS_EXCEPTION(std::exception,readThumbnailOutput(dir.path()));
        }
        save(dir.filePath("thumb.png"),png(96,96));
        QCOMPARE(readThumbnailOutput(dir.path()).rows.at("in/PS/SRV/0").display.size(),QSize(96,96));
    }
    void cancellationAndRetry() {
        QTemporaryDir dir; auto report=fixture(dir.path());
        report["padding"]=std::string(2*1024*1024+17,'x');
        save(dir.filePath("report.json"),QByteArray::fromStdString(report.dump()));
        unsigned checks=0;
        readThumbnailOutput(dir.path(),[&]{++checks;return false;});
        QVERIFY(checks>0);
        qInfo() << "Thumbnail cancellation checkpoints" << checks;
        for (unsigned stop=1;stop<=checks;++stop) {
            unsigned at=0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,readThumbnailOutput(dir.path(),[&]{return ++at==stop;}));
        }
        QCOMPARE(readThumbnailOutput(dir.path()).rows.size(),size_t(4));
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,readWorkerOutput(dir.path(),"draw-resources",[]{return true;}));
    }
};
QTEST_GUILESS_MAIN(ThumbnailOutputTests)
#include "ThumbnailOutputTests.moc"
