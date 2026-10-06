#include "NormalizedPredicateCapture.h"
#include "application/FrameValidation.h"
#include "application/Experiment.h"
#include "application/PredicateInspector.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class NormalizedPredicationTests final : public QObject {
    Q_OBJECT
  private slots:
    void conditionEditHistory() {
        QTemporaryDir dir;
        normalizedPredicateCapture().save(dir.filePath("edit.gpa_frame"));
        Frame frame(dir.filePath("edit.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setSetter(frame, 1200, {{"predicate", 600}, {"predicate_value", 0}});
        auto verify = [&](bool execute) {
            ReplayOptions o;
            o.warp = true;
            o.until = 3000;
            project.apply(frame, o);
            Replay r(frame, o);
            r.run();
            QCOMPARE(firstWord(r, 7), execute ? 9u : 10u);
        };
        verify(false);
        QVERIFY(project.undo());
        verify(true);
        QVERIFY(project.redo());
        verify(false);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            project.setSetter(frame, 1200, {{"predicate", 601}, {"predicate_value", 0}}));
    }
    void controlsAndInspection() {
        for (bool warp : {false, true})
            for (uint32_t value : {0u, 1u}) {
                auto c = normalizedPredicateCapture(value);
                QTemporaryDir dir;
                const auto path = dir.filePath("condition.gpa_frame");
                c.save(path);
                Frame frame(path.toStdWString());
                QCOMPARE(auditNormalizedPredication(frame).size(), size_t(1));
                QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
                ReplayOptions o;
                o.warp = warp;
                o.until = 3000;
                Replay r(frame, o);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    r.run();
                    QCOMPARE(r.output(20).rgba, value ? (std::vector<uint8_t>{0,255,0,255})
                                                    : (std::vector<uint8_t>{0,0,0,255}));
                    QCOMPARE(firstWord(r, 7), value ? 9u : 10u);
                    QCOMPARE(r.counts.at("captured_predicate_conditions"), uint64_t(1));
                    const auto result = inspectPredicate(frame, r, 600);
                    QVERIFY(result["value"].is_null());
                    QCOMPARE(result["status"], nlohmann::json("captured_condition"));
                    QCOMPARE(result["condition_allows_execution"], nlohmann::json(bool(value)));
                    QVERIFY(!result["resource"]["descriptor_available"].get<bool>());
                    // Private readback must not be skipped by the recorded condition.
                    QCOMPARE(r.previewTexture(20).width, 1u);
                }
            }
    }
    void incompleteEvidenceRejected() {
        for (int variant = 0; variant < 16; ++variant) {
            auto c = normalizedPredicateCapture();
            for (auto &e : c.entries) {
                if (e.id == 1201) {
                    const size_t at = size_t(e.offset);
                    if (variant == 0) put(c.bytes, at, Id(999));
                    if (variant == 1) put(c.bytes, at + 8, Id(601));
                    if (variant == 2) put(c.bytes, at + 16, 0u);
                    if (variant == 3) c.bytes[at + 20] ^= 1;
                    if (variant == 4) c.bytes[at + 36] = 2;
                    if (variant == 5) put(c.bytes, at + 37, 8u);
                    if (variant == 6) put(c.bytes, at + 41, Id(0));
                    if (variant == 7) --e.size;
                    if (variant == 8) e.type = 0x3597;
                }
                if (e.id == 1200 && variant == 9) put(c.bytes, size_t(e.offset) + 24, 2u);
                if (e.id == 1200 && variant == 10) put(c.bytes, size_t(e.offset), Id(999));
                if (e.id == 1200 && variant == 14) e.type = 0x34fc;
            }
            if (variant == 11)
                c.add(3100, 7, 0x241, statePack(Id(0), Id(1), Id(600)));
            if (variant == 12) {
                const auto e = *std::find_if(c.entries.begin(), c.entries.end(),
                                             [](const auto &entry) { return entry.id == 1201; });
                const std::vector<uint8_t> raw(c.bytes.begin() + e.offset, c.bytes.begin() + e.offset + e.size);
                c.add(1202, 7, 0x3166, raw);
            }
            if (variant == 13)
                c.add(450, 7, 0x358e, statePack(Id(0), Id(0), int32_t(0), uint8_t(1), 5u, 0u, Id(600)));
            if (variant == 15)
                c.add(3100, 7, 0x3578, statePack(Id(0), Id(0), int32_t(0), uint8_t(1),
                                                16u, 0u, 1u, 0u, 0u, 0u, uint8_t(0), Id(600)));
            QTemporaryDir dir;
            const auto path = dir.filePath("invalid.gpa_frame");
            c.save(path);
            Frame frame(path.toStdWString());
            QVERIFY(auditNormalizedPredication(frame).empty());
            QVERIFY(validateFrame(frame.path())["errors"].get<unsigned>() > 0);
            ReplayOptions o;
            o.warp = true;
            o.until = 1200;
            Replay r(frame, o);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, r.run());
        }
        for (unsigned length = 0; length <= 50; ++length) {
            if (length == 49) continue;
            auto c = normalizedPredicateCapture();
            for (auto &e : c.entries) if (e.id == 1201) e.size = length;
            c.bytes.push_back(0);
            QTemporaryDir dir;
            c.save(dir.filePath("length.gpa_frame"));
            Frame frame(dir.filePath("length.gpa_frame").toStdWString());
            QVERIFY(auditNormalizedPredication(frame).empty());
        }
    }
    void proofDoesNotAuthorizeOtherUses() {
        for (int variant = 0; variant < 3; ++variant) {
            auto c = normalizedPredicateCapture();
            if (variant == 0) c.add(1500, 7, 0x242, statePack(Id(0), Id(1)));
            if (variant == 1) c.add(1500, 7, 0x248, statePack(Id(0), Id(1), Id(600), 1u));
            if (variant == 2) {
                State s{};
                s.predicate = 600;
                c.add(490, 3, 3, snapshot(s));
                c.add(500, 7, 0x37, statePack(Id(490), Id(0), Id(1), 0u, 0u));
            }
            QTemporaryDir dir;
            c.save(dir.filePath("scope.gpa_frame"));
            Frame frame(dir.filePath("scope.gpa_frame").toStdWString());
            QCOMPARE(auditNormalizedPredication(frame).size(), size_t(1));
            QVERIFY(validateFrame(frame.path())["errors"].get<unsigned>() > 0);
            ReplayOptions o;
            o.warp = true;
            o.until = 2000;
            Replay r(frame, o);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, r.run());
        }
    }
    void originals() {
        const auto root = qEnvironmentVariable("FLORA_INITIAL_PREDICATE_CAPTURES");
        if (root.isEmpty()) QSKIP("Set original initial-predicate corpus");
        for (int mode : {0,1,2,3,8,9,10,11}) {
            Frame frame((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
            const auto proofs = auditNormalizedPredication(frame);
            QCOMPARE(proofs.size(), size_t(1));
            QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
            for (bool warp : {false, true}) {
                ReplayOptions o;
                o.warp = warp;
                Replay r(frame, o);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    r.run();
                    auto image = r.output(r.lastOutputResource());
                    QFile expected(root + QString("/%1/native/expected.rgba").arg(mode));
                    QVERIFY(expected.open(QIODevice::ReadOnly));
                    QCOMPARE(QByteArray(reinterpret_cast<const char *>(image.rgba.data()),
                                        qsizetype(image.rgba.size())), expected.readAll());
                    const auto inspected = inspectPredicate(frame, r, proofs.begin()->second.resource);
                    QVERIFY(inspected["value"].is_null());
                    QCOMPARE(inspected["status"], nlohmann::json("captured_condition"));
                }
            }
        }
    }
};
QTEST_GUILESS_MAIN(NormalizedPredicationTests)
#include "NormalizedPredicationTests.moc"
