#include "ApiExportCapture.h"
#include "application/ApiCommands.h"
#include "application/InitializationReferences.h"
#include <QDir>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <Windows.h>
using namespace flora;
using Json = nlohmann::json;
namespace {
QByteArray read(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Fixture read failed"); return f.readAll(); }
void save(const QString &path, const QByteArray &bytes) { QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) throw std::runtime_error("Fixture write failed"); }
std::pair<QByteArray, QByteArray> classic(const Frame &frame, const std::string &text, std::optional<Id> resource, bool gpu) {
    auto selected = Json::array(); QByteArray csv("\xef\xbb\xbf" "id,api,status,wire_type,wire_bytes,referenced_ids\r\n");
    auto quote = [](QString value) { value.replace('"', "\"\""); return '"' + value + '"'; };
    for (const auto &row : inspectCommands(frame)) if ((!gpu || (row["type"] >= 0x31 && row["type"] <= 0x42)) && commandMatches(row, text, resource)) {
        selected.push_back(row); QStringList refs;
        for (const auto &ref : row["references"]) refs << QString::number(ref["id"].get<Id>());
        csv += (QStringList{QString::number(row["id"].get<Id>()), quote(QString::fromStdString(row["name"])),
            QString::fromStdString(row["status"]), QString("0x%1").arg(row["type"].get<uint16_t>(), 0, 16),
            QString::number(row["wire_size"].get<size_t>()), quote(refs.join(' '))}.join(',') + "\r\n").toUtf8();
    }
    Json report{{"frame", QString::fromStdWString(frame.path().filename().wstring()).toStdString()},
        {"filter", {{"text", text}, {"resource", resource ? Json(*resource) : Json(nullptr)}}},
        {"commands", selected}, {"original_initialization_cache", initialFileCache(frame).report()}};
    if (gpu) report["filter"]["gpu_commands"] = true;
    return {QByteArray::fromStdString(report.dump(2) + "\n"), csv};
}
}
class ApiExportTests final : public QObject {
    Q_OBJECT
  private slots:
    void formats_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"all", "gpu", "text", "resource", "combined", "empty", "escaped", "large", "wide"}) QTest::newRow(mode) << QString(mode);
    }
    void formats() {
        QFETCH(QString, mode); QTemporaryDir source, output;
        auto capture = testing::apiExportCapture(mode == "large" ? 5000 : 20);
        if (mode == "wide") capture.add(Id(UINT32_MAX) + 1, 7, 0xffff, {});
        const auto path = source.filePath(QString::fromUtf8("帧.gpa_frame")); capture.save(path); Frame frame(path.toStdWString());
        const std::string text = mode == "empty" ? "NoSuchCommand" : mode == "escaped" ? "\"\\\n中文" : mode == "text" ? "GetData" : "";
        const auto resource = mode == "resource" || mode == "combined" ? std::optional<Id>(2) : std::nullopt;
        const bool gpu = mode == "gpu" || mode == "combined";
        const auto expected = classic(frame, text, resource, gpu);
        exportCommands(frame, output.path().toStdWString(), text, resource, gpu);
        QCOMPARE(read(output.filePath("commands.json")), expected.first);
        QCOMPARE(read(output.filePath("commands.csv")), expected.second);
        if (mode == "text") {
            const auto rows = Json::parse(expected.first.toStdString())["commands"];
            QCOMPARE(rows[0]["query_result"]["status"], Json("unknown_type"));
            QCOMPARE(rows[1]["query_result"]["status"], Json("complete"));
            QCOMPARE(rows[2]["query_result"]["status"], Json("metadata_conflict"));
        }
        const auto evidence = qEnvironmentVariable("FLORA_API_EXPORT_EVIDENCE");
        if (!evidence.isEmpty() && mode == "large") capture.save(evidence + "/large.gpa_frame");
    }
    void cancellation_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"before", "cache", "records", "writing", "before-publish"}) QTest::newRow(mode) << QString(mode);
    }
    void cancellation() {
        QFETCH(QString, mode); QTemporaryDir source, output;
        const auto path = source.filePath("capture.gpa_frame"); testing::apiExportCapture(5000).save(path); Frame frame(path.toStdWString());
        size_t checkpoint = 0;
        if (mode == "writing" || mode == "before-publish") {
            // QSaveFile staging bytes need not be visible through another
            // Windows file handle. Inject at the final JSON flush or final
            // pre-publication check using this input's deterministic sequence.
            size_t complete = 0;
            exportCommands(frame, output.path().toStdWString(), "", {}, false, [&] { ++complete; return false; });
            QVERIFY(complete > frame.entries().size() * 3);
            checkpoint = mode == "writing" ? complete - 2 : complete;
        }
        save(output.filePath("commands.json"), "Old JSON"); save(output.filePath("commands.csv"), "Old CSV");
        size_t calls = 0; bool reached = false;
        const auto cancelled = [&] {
            ++calls;
            bool stop = (mode == "before" && calls == 1) || (mode == "cache" && calls == 3) ||
                        (mode == "records" && calls == frame.entries().size() + 10);
            stop |= checkpoint && calls == checkpoint;
            reached |= stop; return stop;
        };
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, exportCommands(frame, output.path().toStdWString(), "", {}, false, cancelled));
        QVERIFY(reached);
        QCOMPARE(read(output.filePath("commands.json")), QByteArray("Old JSON"));
        QCOMPARE(read(output.filePath("commands.csv")), QByteArray("Old CSV"));
        QCOMPARE(QDir(output.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
        exportCommands(frame, output.path().toStdWString());
        QCOMPARE(Json::parse(read(output.filePath("commands.json")).toStdString())["commands"].size(), size_t(5008));
    }
    void failures_data() {
        QTest::addColumn<QString>("mode");
        for (const auto mode : {"staging", "json-lock", "csv-lock"}) QTest::newRow(mode) << QString(mode);
    }
    void failures() {
        QFETCH(QString, mode); QTemporaryDir source, output;
        const auto path = source.filePath("capture.gpa_frame"); testing::apiExportCapture().save(path); Frame frame(path.toStdWString());
        save(output.filePath("commands.json"), "Old JSON");
        if (mode == "staging") QVERIFY(QDir(output.path()).mkdir("commands.csv"));
        else save(output.filePath("commands.csv"), "Old CSV");
        HANDLE lock = INVALID_HANDLE_VALUE;
        auto release = qScopeGuard([&] { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); });
        if (mode != "staging") {
            const auto target = output.filePath(mode == "json-lock" ? "commands.json" : "commands.csv");
            lock = CreateFileW(reinterpret_cast<const wchar_t *>(target.utf16()), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr); QVERIFY(lock != INVALID_HANDLE_VALUE);
        }
        QString error;
        try { exportCommands(frame, output.path().toStdWString()); } catch (const std::runtime_error &e) { error = QString::fromUtf8(e.what()); }
        QVERIFY(!error.isEmpty());
        if (mode == "csv-lock") { QVERIFY(error.contains("partially published")); QVERIFY(Json::parse(read(output.filePath("commands.json")).toStdString()).contains("commands")); }
        else QCOMPARE(read(output.filePath("commands.json")), QByteArray("Old JSON"));
        if (mode != "staging") QCOMPARE(read(output.filePath("commands.csv")), QByteArray("Old CSV"));
        if (lock != INVALID_HANDLE_VALUE) { CloseHandle(lock); lock = INVALID_HANDLE_VALUE; }
        if (mode == "staging") QVERIFY(QDir(output.path()).rmdir("commands.csv"));
        exportCommands(frame, output.path().toStdWString());
        const auto expected = classic(frame, "", {}, false);
        QCOMPARE(read(output.filePath("commands.json")), expected.first); QCOMPARE(read(output.filePath("commands.csv")), expected.second);
    }
    void inspectionCancellation() {
        QTemporaryDir source; const auto path = source.filePath("capture.gpa_frame"); testing::apiExportCapture().save(path); Frame frame(path.toStdWString());
        size_t calls = 0;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, inspectCommands(frame, [&] { return ++calls > frame.entries().size() + 5; }));
        QCOMPARE(inspectCommands(frame).size(), size_t(28));
    }
    void cacheReportCancellation() {
        QTemporaryDir source; const auto path = source.filePath("capture.gpa_frame"); testing::apiExportCapture().save(path); Frame frame(path.toStdWString());
        const auto cache = initialFileCache(frame); int calls = 0;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, cache.report([&] { return ++calls == 4; }));
        QVERIFY(cache.report()["categories"].size() > 4);
    }
};
QTEST_GUILESS_MAIN(ApiExportTests)
#include "ApiExportTests.moc"
