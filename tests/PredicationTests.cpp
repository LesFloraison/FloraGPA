#include "PredicateCapture.h"
#include "StreamCapture.h"
#include "application/PredicateInspector.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
class PredicationTests final : public QObject {
    Q_OBJECT
  private slots:
    void checkedCommands() {
        for (auto type : {0x241, 0x30b2, 0x31b2, 0x331b, 0x33e1, 0x34f9, 0x243, 0x30b3, 0x31b3, 0x331c,
                          0x33e2, 0x34fa, 0x248, 0x30b5, 0x31b5, 0x331e, 0x33e4, 0x34fc}) {
            auto bytes = statePack(Id(0), Id(1), Id(600));
            if (predicateOperation(uint16_t(type)) == PredicateOperation::Set)
                append(bytes, UINT32_MAX);
            auto command = readPredicateCommand(uint16_t(type), bytes);
            QCOMPARE(command.resource, Id(600));
            QCOMPARE(command.context, Id(1));
            for (size_t n = 0; n < bytes.size(); ++n)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                         readPredicateCommand(uint16_t(type), Bytes(bytes.data(), n)));
            bytes.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPredicateCommand(uint16_t(type), bytes));
        }
    }
    void activeResetAndClearState() {
        auto c = predicateCapture();
        c.add(1050, 7, 0x242, statePack(Id(0), Id(1)));
        QTemporaryDir dir;
        c.save(dir.path() + "/active.gpa_frame");
        Frame frame((dir.path() + "/active.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = true;
        options.until = 1050;
        Replay replay(frame, options);
        for (int run = 0; run < 3; ++run) {
            replay.run();
            auto result = replay.readPredicateResult(600);
            QCOMPARE(result.status, std::string("active"));
            QVERIFY(!result.bound);
        }
    }
    void streamOverflowIsolation() {
        for (bool overflow : {false, true}) {
            auto c = streamCapture();
            if (overflow)
                for (auto &entry : c.entries) {
                    if (entry.id == 70)
                        put(c.bytes, size_t(entry.offset) + 16, 64u);
                    if (entry.id == 71) {
                        put(c.bytes, size_t(entry.offset), 64u);
                        entry.size = 68;
                    }
                }
            c.add(600, 5, 0x96, statePack(Id(0), Id(0), 7u, 0u));
            c.add(85, 7, 0x241, statePack(Id(0), Id(1), Id(600)));
            c.add(160, 7, 0x243, statePack(Id(0), Id(1), Id(600)));
            QTemporaryDir dir;
            c.save(dir.path() + "/overflow.gpa_frame");
            Frame frame((dir.path() + "/overflow.gpa_frame").toStdWString());
            ReplayOptions o;
            o.warp = true;
            o.until = 160;
            Replay r(frame, o);
            QVERIFY_THROWS_NO_EXCEPTION(r.run({}, [&](Id id, bool after, auto *, const auto &) {
                if (id == 100 && after)
                    r.previewTexture(20);
            }));
            QCOMPARE(r.readPredicateResult(600).value, std::optional<bool>(overflow));
        }
    }
    void replay_data() {
        QTest::addColumn<bool>("warp");
        QTest::addColumn<bool>("visible");
        QTest::addColumn<uint32_t>("value");
        QTest::addColumn<bool>("helper");
        for (bool warp : {false, true})
            for (bool visible : {false, true})
                for (uint32_t value : {0u, 1u, 7u, UINT32_MAX})
                    for (bool helper : {false, true})
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(warp).arg(visible).arg(value).arg(helper)))
                            << warp << visible << value << helper;
    }
    void replay() {
        QFETCH(bool, warp);
        QFETCH(bool, visible);
        QFETCH(uint32_t, value);
        QFETCH(bool, helper);
        auto capture = predicateCapture(visible, value);
        QTemporaryDir dir;
        capture.save(dir.path() + "/predicate.gpa_frame");
        Frame frame((dir.path() + "/predicate.gpa_frame").toStdWString());
        ReplayOptions options;
        options.warp = warp;
        options.until = 3000;
        Replay replay(frame, options);
        // Preserve raw BOOL: the Python oracle confirms WARP compares the full
        // value here, while this hardware interprets nonzero as TRUE.
        bool skip = warp ? uint32_t(visible) == value : visible == bool(value);
        for (int repeat = 0; repeat < 2; ++repeat) {
            replay.run({}, [&](Id event, bool after, auto *, const auto &) {
                if (event == 1000 && after) {
                    auto active = replay.readPredicateResult(600);
                    QCOMPARE(active.status, std::string("active"));
                    QVERIFY(!active.value);
                    if (helper)
                        QCOMPARE(replay.previewTexture(20).width, 1u);
                }
            });
            auto result = replay.readPredicateResult(600);
            QCOMPARE(result.status, std::string("ready"));
            QCOMPARE(result.value, std::optional<bool>(visible));
            QVERIFY(result.bound);
            QCOMPARE(result.predicateValue, value);
            QCOMPARE(replay.output(20).rgba, (skip ? (visible ? std::vector<uint8_t>{255, 0, 0, 255}
                                                              : std::vector<uint8_t>{0, 0, 0, 255})
                                                   : std::vector<uint8_t>{0, 255, 0, 255}));
            QCOMPARE(firstWord(replay, 7), skip ? 10u : 9u);
            auto json = inspectPredicate(frame, replay, 600);
            QCOMPARE(json["source"], nlohmann::json("replayed_gpu_query"));
            QVERIFY(!json["captured_result_restored"].get<bool>());
        }
    }
    void hintIsolation() {
        for (bool visible : {false, true}) {
            auto c = predicateCapture(visible, 0, true);
            QTemporaryDir dir;
            c.save(dir.path() + "/hint.gpa_frame");
            Frame frame((dir.path() + "/hint.gpa_frame").toStdWString());
            ReplayOptions o;
            o.warp = true;
            o.until = 3000;
            Replay r(frame, o);
            r.run({}, [&](Id id, bool after, auto *, const auto &) {
                if (id == 1000 && after)
                    r.previewTexture(20);
            });
            auto result = r.readPredicateResult(600);
            QCOMPARE(result.status, std::string("hint_result_unavailable"));
            QVERIFY(!result.value);
            QCOMPARE(r.output(20).rgba,
                     visible ? (std::vector<uint8_t>{0, 255, 0, 255}) : (std::vector<uint8_t>{0, 0, 0, 255}));
        }
    }
    void editedSkippedDispatch() {
        auto c = predicateCapture(true, 1);
        QTemporaryDir dir;
        c.save(dir.path() + "/edit.gpa_frame");
        Frame frame((dir.path() + "/edit.gpa_frame").toStdWString());
        ReplayOptions o;
        o.warp = true;
        o.until = 3000;
        o.buffers[3000][7].push_back({0, {21, 0, 0, 0}});
        Replay r(frame, o);
        r.run();
        QCOMPARE(firstWord(r, 7), 21u);
        o.before = true;
        Replay before(frame, o);
        before.run();
        QCOMPARE(firstWord(before, 7), 10u);
        before.inspectEventInputs(3000, [&] { QCOMPARE(firstWord(before, 7), 21u); });
        QCOMPARE(firstWord(before, 7), 10u);
    }
};
QTEST_GUILESS_MAIN(PredicationTests)
#include "PredicationTests.moc"
