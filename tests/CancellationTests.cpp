#include "NormalizedPredicateCapture.h"
#include "SyntheticCapture.h"
#include "application/FrameValidation.h"
#include "core/BufferCreation.h"
#include "core/ClassCreation.h"
#include "core/MapRecords.h"
#include "core/PipelineCreation.h"
#include "core/PredicateCreation.h"
#include "core/ResourceLod.h"
#include "core/TextureCreation.h"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest>

using namespace flora;
namespace {
// Exercise nested identity/creation scans, unused-object state scans, all LOD
// passes, Map pairing and normalized-predicate proof. No GPU is created here.
testing::Capture semanticCapture() {
    using namespace testing;
    auto c = normalizedPredicateCapture();
    c.add(10000, 5, 0x81, std::vector<uint8_t>(28));
    c.add(10001, 5, 0x97, statePack(Id(0), Id(10000)));
    D3D11_TEXTURE2D_DESC desc{4,
                              4,
                              1,
                              1,
                              DXGI_FORMAT_R8G8B8A8_UNORM,
                              {1, 0},
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE,
                              0,
                              D3D11_RESOURCE_MISC_RESOURCE_CLAMP};
    c.add(10002, 5, 0x85, statePack(Id(0), Id(10000), desc, Id(0)));
    c.add(10010, 7, 0x3516, statePack(Id(0), Id(1), 0.f, Id(10002)));
    c.add(10011, 7, 0x3515, statePack(Id(0), Id(1), Id(10002), 0.f));
    c.add(10012, 7, 0x357a, statePack(Id(0), Id(10000), int32_t(-1), uint8_t(0), uint8_t(0), Id(0)));
    c.add(10013, 7, 0x3578, statePack(Id(0), Id(10000), int32_t(-1), uint8_t(0), uint8_t(0), Id(0)));
    c.add(10014, 7, 0x358e, statePack(Id(0), Id(10000), int32_t(-1), uint8_t(0), Id(0)));
    c.add(10015, 7, 0x3195, statePack(Id(0), Id(10001), int32_t(0), 0u, Id(20000)));
    auto code = compileClassProgram("float4 main():SV_Position{return 0;}", "vs_5_0");
    auto layout = statePack(Id(0), Id(10000), int32_t(0), 0u, uint8_t(1), uint64_t(code.size()), uint8_t(1));
    layout.insert(layout.end(), code.begin(), code.end());
    append(layout, Id(20001));
    c.add(10016, 7, 0x3580, layout);
    c.add(10017, 7, 0x34ec, statePack(Id(0), Id(1), int32_t(0), Id(2), 0u, 1u, 0u, Id(777)));
    c.add(10018, 7, 0x34ed, statePack(Id(0), Id(1), Id(2), 0u));
    return c;
}
} // namespace
class CancellationTests final : public QObject {
    Q_OBJECT
    template <class F> bool cancelled(F f) {
        try {
            f();
        } catch (const OperationCancelled &) {
            return true;
        }
        return false;
    }
  private slots:
    void semanticAuditCheckpoints() {
        QTemporaryDir dir;
        const auto path = dir.filePath("semantic.gpa_frame");
        semanticCapture().save(path);
        Frame frame(path.toStdWString());
        QVERIFY(!auditClassCreations(frame).records.at(10015).note.empty());
        QVERIFY(auditClassCreations(frame).records.at(10015).error.empty());
        QVERIFY(!auditPipelineCreations(frame).records.at(10016).note.empty());
        QVERIFY(auditPipelineCreations(frame).records.at(10016).error.empty());
        QCOMPARE(auditNormalizedPredication(frame).size(), size_t(1));
        QCOMPARE(auditResourceLod(frame).initial.size(), size_t(1));
        QCOMPARE(auditMapRecords(frame).at(10017).pairedEvent, Id(10018));
        const std::array<std::function<void(const Frame &, const CancelCheck &)>, 9> audits{
            auditBufferCreations, auditTextureCreations,   auditClassCreations,
            auditClassIdentities, auditPredicateCreations, auditPipelineCreations,
            auditMapRecords,      auditResourceLod,        auditNormalizedPredication};
        testing::Capture empty;
        empty.save(dir.filePath("empty.gpa_frame"));
        Frame emptyFrame(dir.filePath("empty.gpa_frame").toStdWString());
        for (const auto &audit : audits) {
            QVERIFY(cancelled([&] { audit(emptyFrame, [] { return true; }); }));
            unsigned checkpoints = 0;
            audit(frame, [&] {
                ++checkpoints;
                return false;
            });
            QVERIFY(checkpoints > frame.entries().size());
            for (unsigned stop = 1; stop <= checkpoints; ++stop) {
                unsigned calls = 0;
                QVERIFY2(cancelled([&] { audit(frame, [&] { return ++calls == stop; }); }),
                         qPrintable(
                             QString("Checkpoint %1/%2 swallowed cancellation").arg(stop).arg(checkpoints)));
                QCOMPARE(calls, stop); // No subsequent scan after the interruption.
            }
            audit(frame, {}); // Same mapped capture remains retryable.
        }
    }
    void semanticPreflightCheckpoints_data() {
        QTest::addColumn<int>("variant");
        QTest::newRow("valid") << 0;
        QTest::newRow("truncated") << 1;
        QTest::newRow("invalid-reference") << 2;
        QTest::newRow("illegal-length") << 3;
        QTest::newRow("unpaired-map") << 4;
    }
    void semanticPreflightCheckpoints() {
        QFETCH(int, variant);
        QTemporaryDir dir;
        const auto path = dir.filePath("semantic.gpa_frame");
        auto c = semanticCapture();
        for (auto &e : c.entries) {
            if (variant == 1 && e.id == 10013)
                --e.size;
            if (variant == 2 && e.id == 10011)
                testing::put(c.bytes, size_t(e.offset) + 16, Id(999999));
            if (variant == 3 && e.id == 10016)
                testing::put(c.bytes, size_t(e.offset) + 25, UINT64_MAX);
        }
        if (variant == 4)
            c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                           [](const auto &e) { return e.id == 10018; }),
                            c.entries.end());
        c.save(path);
        const auto baseline = validateFrame(path.toStdWString());
        QCOMPARE(baseline["completed"], nlohmann::json(true));
        QCOMPARE(baseline["errors"].get<unsigned>() == 0, variant == 0);
        unsigned checkpoints = 0;
        QCOMPARE(validateFrame(path.toStdWString(),
                               [&] {
                                   ++checkpoints;
                                   return false;
                               }),
                 baseline);
        QVERIFY(checkpoints > c.entries.size() * 5); // Bulk semantic passes were reached.
        DWORD before{}, after{};
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &before));
        for (unsigned stop = 1; stop <= checkpoints; ++stop) {
            unsigned calls = 0;
            auto report = validateFrame(path.toStdWString(), [&] { return ++calls == stop; });
            QCOMPARE(calls, stop);
            QCOMPARE(report["status"], nlohmann::json("cancelled"));
            QCOMPARE(report["completed"], nlohmann::json(false));
            QCOMPARE(report["gpu_validation"], nlohmann::json("not_run"));
            QCOMPARE(report["replay_proven"], nlohmann::json(false));
            // Preserve genuine earlier findings, never turn cancellation into a
            // corrupt record/container or fabricate errors absent from the baseline.
            for (const auto &finding : report["findings"])
                QVERIFY(std::find(baseline["findings"].begin(), baseline["findings"].end(), finding) !=
                        baseline["findings"].end());
            QFile writable(path);
            QVERIFY(writable.open(QIODevice::ReadWrite));
        }
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &after));
        QCOMPARE(after, before);
        QCOMPARE(validateFrame(path.toStdWString()), baseline);
        qInfo("Checked %u cancellation checkpoints and exact retry", checkpoints);
    }
    void parseCancellationReleasesMapping() {
        QTemporaryDir dir;
        const auto path = dir.filePath("many.gpa_frame");
        testing::Capture c;
        for (Id id = 1; id <= 4096; ++id)
            c.add(id, 9, 1, testing::word(0));
        c.save(path);
        DWORD before{}, after{};
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &before));
        for (int repeat = 0; repeat < 100; ++repeat) {
            unsigned calls = 0;
            QVERIFY(cancelled([&] { Frame frame(path.toStdWString(), [&] { return ++calls == 3; }); }));
            QCOMPARE(calls, 3u); // Initial checkpoint, then entries 0 and 1024.
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite)); // No abandoned read-only mapping/handle.
        }
        QVERIFY(GetProcessHandleCount(GetCurrentProcess(), &after));
        QCOMPARE(after, before);
        Frame frame(path.toStdWString());
        QCOMPARE(frame.entries().size(), size_t(4096));
    }
    void hashCancellationAndRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("large.gpa_frame");
        testing::Capture c;
        c.add(1, 9, 1, testing::word(0));
        c.save(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(48 * 1024 * 1024 + 17));
        const auto bytes = file.readAll();
        const auto expected = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        file.close();
        Frame frame(path.toStdWString());
        unsigned calls = 0;
        QVERIFY(cancelled([&] { frame.sha256([&] { return ++calls == 4; }); }));
        QCOMPARE(calls, 4u); // Stop after one 16 MiB block, before consuming the rest.
        QCOMPARE(QString::fromStdString(frame.sha256()), QString::fromLatin1(expected));
        QVERIFY(cancelled([&] { frame.sha256([] { return true; }); }));
        QCOMPARE(QString::fromStdString(frame.sha256()), QString::fromLatin1(expected));
        QCOMPARE(QString::fromStdString(sha256({})),
                 QString("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }
    void preflightCancellationIsNotCorruption() {
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        testing::Capture c;
        c.add(1, 9, 1, testing::word(0));
        c.save(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.resize(32 * 1024 * 1024));
        file.close();
        unsigned calls = 0;
        auto report = validateFrame(path.toStdWString(), [&] { return ++calls == 7; });
        QCOMPARE(report["status"], nlohmann::json("cancelled"));
        QCOMPARE(report["completed"], nlohmann::json(false));
        QCOMPARE(report["errors"], nlohmann::json(0));
        QVERIFY(report["findings"].empty());
        QVERIFY(!report.contains("source_sha256"));
        QCOMPARE(validateFrame(path.toStdWString())["status"], nlohmann::json("checked"));
        auto absent = validateFrame(dir.filePath("absent").toStdWString(), [] { return true; });
        QCOMPARE(absent["status"], nlohmann::json("cancelled"));
        QCOMPARE(absent["errors"], nlohmann::json(0));
    }
};
QTEST_GUILESS_MAIN(CancellationTests)
#include "CancellationTests.moc"
