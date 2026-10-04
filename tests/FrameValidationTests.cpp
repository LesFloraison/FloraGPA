#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/ConstantBufferBindings.h"
#include "core/ReplayCapabilities.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class FrameValidationTests final : public QObject {
    Q_OBJECT
  private slots:
    void constantBufferReferences_data() {
        QTest::addColumn<unsigned>("type");
        QTest::addColumn<int>("variant");
        for (unsigned stage = 0; stage < 6; ++stage)
            for (unsigned type : {0x249 + stage, 0x24f + stage, unsigned(constantBufferShimTypes[stage])})
                for (int variant = 0; variant < 9; ++variant)
                    QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(variant))) << type << variant;
    }
    void constantBufferReferences() {
        QFETCH(unsigned, type);
        QFETCH(int, variant);
        QTemporaryDir dir;
        Capture c;
        std::vector<uint8_t> context(24);
        if (variant == 8)
            context[16] = 1; // Deferred context cannot execute an immediate setter.
        c.add(1, 5, 0x127, context);
        if (variant != 2) {
            if (variant == 3)
                c.add(2, 5, 0x127, std::vector<uint8_t>(24));
            else
                c.buffer(2, 3, variant == 4 ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_CONSTANT_BUFFER, 0,
                         {0, 0, 0, 0});
        }
        std::vector<uint8_t> raw;
        append(raw, Id(0));
        append(raw, Id(1));
        append(raw, variant == 6 ? 14u : 0u);
        append(raw, 1u);
        append(raw, uint8_t(1));
        append(raw, variant == 1 ? Id(0) : Id(2));
        if (type >= 0x24f && type <= 0x254) {
            append(raw, uint8_t(1));
            append(raw, 0u);
            append(raw, uint8_t(variant != 7));
            if (variant != 7)
                append(raw, 16u);
        } else if (variant == 7)
            raw.push_back(0); // Non-CB1 records reject trailing data.
        if (variant == 5)
            raw.pop_back();
        c.add(100, 7, uint16_t(type), raw);
        const auto path = dir.filePath("cb.gpa_frame");
        c.save(path);
        const auto report = validateFrame(path.toStdWString());
        QCOMPARE(report["status"] == "blocked", variant >= 2);
        QCOMPARE(report["gpu_validation"], nlohmann::json("not_run"));
        if (variant == 2 || variant == 3) {
            bool located = false;
            for (const auto &finding : report["findings"])
                located |= finding["kind"] == "constant_buffer_resource_unresolved" &&
                           finding["event_id"] == 100 && finding["resource_id"] == 2 &&
                           finding["record_type"] == type;
            QVERIFY(located);
        }
    }
    void malformedContainer() {
        QTemporaryDir dir;
        auto result = validateFrame(dir.filePath("missing.gpa_frame").toStdWString());
        QCOMPARE(result["status"], nlohmann::json("blocked"));
        QCOMPARE(result["completed"], nlohmann::json(false));
        QCOMPARE(result["findings"][0]["kind"], nlohmann::json("container_rejected"));
    }
    void checkedDoesNotMeanReplayed() {
        QTemporaryDir dir;
        Capture c;
        c.buffer(1, 2, D3D11_BIND_VERTEX_BUFFER, 0, {1, 2, 3, 4});
        std::vector<uint8_t> raw(16);
        append(raw, Id(UINT64_MAX));
        c.add(UINT64_MAX - 1, 7, 0x359d, raw);
        c.save(dir.filePath("good.gpa_frame"));
        auto result = validateFrame(dir.filePath("good.gpa_frame").toStdWString());
        QCOMPARE(result["status"], nlohmann::json("checked"));
        QCOMPARE(result["gpu_validation"], nlohmann::json("not_run"));
        QCOMPARE(result["replay_proven"], nlohmann::json(false));
        QCOMPARE(result["scanned_records"], nlohmann::json(3));
        for (auto &row : result["coverage"])
            if (row["category"] == 7)
                QCOMPARE(row["first_entry"], nlohmann::json(UINT64_MAX - 1));
    }
    void allFailuresAreRetained() {
        QTemporaryDir dir;
        Capture c;
        c.add(10, 7, 0xffff, std::vector<uint8_t>(16));
        c.add(11, 7, 0x359d, std::vector<uint8_t>(23));
        auto raw = std::vector<uint8_t>(16);
        append(raw, 0);
        append(raw, uint8_t(1));
        append(raw, 0u);
        append(raw, 0u);
        append(raw, Id(55));
        c.add(12, 7, 0x358d, raw);
        c.add(13, 3, 3, std::vector<uint8_t>(20));
        c.save(dir.filePath("bad.gpa_frame"));
        auto r = validateFrame(dir.filePath("bad.gpa_frame").toStdWString());
        QCOMPARE(r["scanned_records"], nlohmann::json(4));
        QVERIFY(r["errors"].get<int>() >= 4);
        std::set<Id> ids;
        for (auto &f : r["findings"])
            ids.insert(f["entry_id"].get<Id>());
        QCOMPARE(ids, std::set<Id>({10, 11, 12, 13}));
    }
    void explicitHandlingOnly() {
        QCOMPARE(std::string(replayCapability(0x359d).handling), std::string("metadata"));
        QCOMPARE(std::string(replayCapability(0x41).handling), std::string("unsupported"));
        QCOMPARE(std::string(replayCapability(0x34f6).handling), std::string("execute"));
        QCOMPARE(std::string(replayCapability(0x3017).handling), std::string("metadata"));
        QCOMPARE(std::string(replayCapability(0x3578).handling), std::string("execute"));
    }
    void formerFallbackRecordsCannotSkipValidation() {
        QTemporaryDir dir;
        std::vector<std::pair<uint16_t, std::vector<uint8_t>>> records;
        auto prefix = [] {
            std::vector<uint8_t> raw;
            append(raw, Id(0));
            append(raw, Id(1));
            return raw;
        };
        for (uint16_t type : {0x34e7, 0x34e9, 0x34f5, 0x351a, 0x351e, 0x3523}) {
            auto raw = prefix();
            append(raw, Id(0));
            append(raw, 0u);
            append(raw, uint8_t(0));
            records.emplace_back(type, raw);
        }
        auto raw = prefix();
        append(raw, 4u);
        records.emplace_back(0x34f6, raw);
        raw = prefix();
        append(raw, Id(0));
        append(raw, uint8_t(0));
        append(raw, UINT32_MAX);
        records.emplace_back(0x3501, raw);
        raw = prefix();
        append(raw, Id(0));
        append(raw, 0u);
        records.emplace_back(0x3502, raw);
        raw = prefix();
        append(raw, Id(0));
        records.emplace_back(0x3509, raw);
        raw = prefix();
        append(raw, 0u);
        append(raw, uint8_t(0));
        records.emplace_back(0x350a, raw);
        raw = prefix();
        append(raw, 1u);
        append(raw, Id(0));
        append(raw, uint8_t(0));
        append(raw, 0u);
        append(raw, 0u);
        records.emplace_back(0x34fb, raw);
        for (const auto &[type, payload] : records)
            for (bool truncate : {false, true}) {
                Capture c;
                c.add(1, 5, 0x127, std::vector<uint8_t>(24));
                auto bytes = payload;
                if (truncate)
                    bytes.pop_back();
                c.add(100, 7, type, bytes);
                const auto path = dir.filePath(QString("%1-%2.gpa_frame").arg(type).arg(truncate));
                c.save(path);
                const auto report = validateFrame(path.toStdWString());
                QCOMPARE(report["status"] == "blocked", truncate);
                Frame frame(path.toStdWString());
                ReplayOptions options;
                options.warp = true;
                Replay replay(frame, options);
                if (truncate)
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
                else
                    replay.run();
            }
        Capture unknown;
        unknown.add(1, 5, 0x127, std::vector<uint8_t>(24));
        unknown.add(100, 7, 0xfffe, prefix());
        const auto path = dir.filePath("unknown.gpa_frame");
        unknown.save(path);
        Frame frame(path.toStdWString());
        QCOMPARE(validateFrame(path.toStdWString())["status"], nlohmann::json("blocked"));
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
    }
    void extendedOutputSlots() {
        QTemporaryDir dir;
        for (unsigned count : {9u, 64u, 65u}) {
            Capture c;
            State s{};
            s.omStart = 8;
            s.rtCount = count;
            c.add(1, 3, 3, snapshot(s));
            auto path = dir.filePath(QString::number(count) + ".gpa_frame");
            c.save(path);
            QCOMPARE(validateFrame(path.toStdWString())["status"],
                     nlohmann::json(count <= 64 ? "checked" : "blocked"));
        }
    }
    void unresolvedReferenceIsLocatedButNotCertified() {
        QTemporaryDir dir;
        Capture c;
        State s{};
        s.stages[0].shader = 9876543210;
        c.add(77, 3, 3, snapshot(s));
        auto path = dir.filePath("reference.gpa_frame");
        c.save(path);
        const auto r = validateFrame(path.toStdWString());
        QCOMPARE(r["status"], nlohmann::json("review_required"));
        QCOMPARE(r["findings"][0]["entry_id"], nlohmann::json(77));
        QCOMPARE(r["findings"][0]["resource_id"], nlohmann::json(9876543210ull));
        QCOMPARE(r["replay_proven"], nlohmann::json(false));
    }
    void cancelAndRetry() {
        QTemporaryDir dir;
        Capture c;
        c.buffer(1, 2, 0, 0, {0, 0, 0, 0});
        c.save(dir.filePath("cancel.gpa_frame"));
        auto p = dir.filePath("cancel.gpa_frame").toStdWString();
        QCOMPARE(validateFrame(p, [] { return true; })["status"], nlohmann::json("cancelled"));
        QCOMPARE(validateFrame(p)["status"], nlohmann::json("checked"));
    }
    void cliReportAndOutputIsolation() {
        QTemporaryDir dir;
        const auto out = dir.filePath("report");
        QProcess process;
        auto invoke = [&](QStringList extra) {
            QStringList args{"validate-frame", dir.filePath("missing.gpa_frame"), "--out", out};
            args.append(extra);
            process.start(QCoreApplication::applicationDirPath() + "/FloraGPA.Cli.exe", args);
            return process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit;
        };
        QVERIFY(invoke({}));
        QCOMPARE(process.exitCode(), 2);
        QFile file(out + "/validation.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll();
        file.close();
        QCOMPARE(nlohmann::json::parse(original.constData())["status"], nlohmann::json("blocked"));
        QVERIFY(invoke({}));
        QVERIFY(process.exitCode() != 0);
        QVERIFY(process.readAllStandardError().contains("new or empty"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
        file.close();
        QVERIFY(invoke({"--warp"}));
        QVERIFY(process.exitCode() != 0);
        QVERIFY(process.readAllStandardError().contains("accepts only --out"));
    }
};
QTEST_GUILESS_MAIN(FrameValidationTests)
#include "FrameValidationTests.moc"
