#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "application/PredicateInspector.h"
#include "core/InspectionRecords.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
template <class... T> Raw pack(T... v) {
    Raw b;
    (append(b, v), ...);
    return b;
}
Raw copy(Bytes b) { return {b.begin(), b.end()}; }
} // namespace
class PredicateCreationTests final : public QObject {
    Q_OBJECT
  private slots:
    void originalCaptures_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode = 0; mode < 10; mode++)
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originalCaptures() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        auto root = qEnvironmentVariable("FLORA_PREDICATE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        Frame frame((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
        auto a = auditPredicateCreations(frame);
        QCOMPARE(validateFrame(frame.path())["errors"], nlohmann::json(0));
        for (const auto &[id, c] : a.records) {
            QVERIFY2(c.error.empty(), c.error.c_str());
            QCOMPARE(inspectCommand(frame, id)["status"], nlohmann::json("decoded"));
        }
        ReplayOptions o;
        o.warp = warp;
        Replay replay(frame, o);
        Id finalDraw = 0;
        PredicateCommand binding{};
        for (const auto &[id, e] : frame.entries()) {
            if (e.category != 7)
                continue;
            if (predicateOperation(e.type) == PredicateOperation::Set) {
                const auto c = readPredicateCommand(e.type, frame.payload(id));
                if (c.resource)
                    binding = c;
            }
            if (e.type == 0x37)
                finalDraw = id;
        }
        QVERIFY(finalDraw && binding.resource);
        for (int repeat = 0; repeat < 2; repeat++) {
            // No observer/readback may mask a broken production replay path.
            replay.run();
            auto final = replay.output(replay.lastOutputResource());
            QFile oracle(root + QString("/%1/native/expected.rgba").arg(mode));
            QVERIFY(oracle.open(QIODevice::ReadOnly));
            QCOMPARE(
                QByteArray(reinterpret_cast<const char *>(final.rgba.data()), qsizetype(final.rgba.size())),
                oracle.readAll());
            unsigned creates = 0, ends = 0, draws = 0, conditionalChecks = 0;
            replay.run({}, {}, [&](Id event, bool after, auto *ctx, const auto &objects) {
                if (!after)
                    return;
                const auto &e = frame.entry(event);
                if (e.type == 0x358e) {
                    const auto &c = a.records.at(event);
                    if (c.result) {
                        QVERIFY(!objects.contains(c.resource));
                        return;
                    }
                    ++creates;
                    QVERIFY(objects.contains(c.resource));
                    Com<ID3D11Predicate> p;
                    QVERIFY(SUCCEEDED(objects.at(c.resource).As(&p)));
                    D3D11_QUERY_DESC desc{};
                    p->GetDesc(&desc);
                    QCOMPARE(uint32_t(desc.Query), c.type);
                    QCOMPARE(desc.MiscFlags, c.flags);
                    if (frame.entries().contains(c.resource))
                        QCOMPARE(replay.readPredicateResult(c.resource).status, std::string("unissued"));
                    if (frame.entries().contains(c.resource))
                        QCOMPARE(describePredicate(frame, c.resource)["initial_result_source"],
                                 nlohmann::json("unissued_frame_time_creation"));
                }
                if (predicateOperation(e.type) == PredicateOperation::Begin) {
                    auto c = readPredicateCommand(e.type, frame.payload(event));
                    QCOMPARE(replay.readPredicateResult(c.resource).status, std::string("active"));
                }
                if (predicateOperation(e.type) == PredicateOperation::End) {
                    auto c = readPredicateCommand(e.type, frame.payload(event));
                    auto result = replay.readPredicateResult(c.resource, 5000);
                    if (mode == 4)
                        QCOMPARE(result.status, std::string("hint_result_unavailable"));
                    else {
                        QCOMPARE(result.status, std::string("ready"));
                        QCOMPARE(result.value, std::optional<bool>(mode != 0 && mode != 3 && mode != 8 &&
                                                                   !(mode == 9 && ends == 1)));
                    }
                    ++ends;
                }
                if (e.type == 0x37) {
                    ++draws;
                    if (event == finalDraw) {
                        ++conditionalChecks;
                        Com<ID3D11Predicate> p;
                        BOOL value = FALSE;
                        ctx->GetPredication(&p, &value);
                        QCOMPARE(static_cast<IUnknown *>(p.Get()), objects.at(binding.resource).Get());
                        QCOMPARE(uint32_t(value), binding.value);
                        auto image = replay.output(replay.lastOutputResource());
                        QFile expected(root + QString("/%1/native/expected.rgba").arg(mode));
                        QVERIFY(expected.open(QIODevice::ReadOnly));
                        QCOMPARE(QByteArray(reinterpret_cast<const char *>(image.rgba.data()),
                                            qsizetype(image.rgba.size())),
                                 expected.readAll());
                    }
                }
            });
            QCOMPARE(creates, mode == 7 ? 2u : 1u);
            QCOMPARE(ends, mode == 9 ? 2u : 1u);
            QCOMPARE(draws, mode == 9 ? 3u : 2u);
            QCOMPARE(conditionalChecks, 1u);
        }
    }
    void wireAndMetadataBounds() {
        auto b = pack(Id(0), Id(3), int32_t(0), uint8_t(1), 5u, 0u, Id(5));
        QCOMPARE(readPredicateCreation(b).resource, Id(5));
        for (size_t n = 0; n < b.size(); n++)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPredicateCreation(Bytes(b).first(n)));
        auto bad = b;
        bad.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPredicateCreation(bad));
        bad = b;
        bad[20] = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPredicateCreation(bad));
        for (uint16_t t : {0x3162, 0x3163, 0x3164, 0x3166, 0x3167, 0x3169, 0x316a}) {
            Raw raw = pack(Id(0), Id(5));
            if (t == 0x3162 || t == 0x3166 || t == 0x3167) {
                append(raw, int32_t(0));
                append(raw, std::array<uint8_t, 16>{});
                if (t == 0x3166)
                    append(raw, uint8_t(1));
                if (t != 0x3162)
                    append(raw, 4u);
                append(raw, Id(123));
            } else if (t == 0x316a) {
                append(raw, uint8_t(1));
                append(raw, 5u);
                append(raw, 0u);
            } else
                append(raw, 4u);
            auto accept = [&](Bytes bytes) {
                if (t == 0x3167)
                    readPrivateDataObservation(bytes);
                else if (!acceptPassiveObjectRecord(t, bytes))
                    throw std::runtime_error("Not accepted");
            };
            accept(raw);
            for (size_t n = 0; n < raw.size(); n++)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, accept(Bytes(raw).first(n)));
            raw.push_back(0);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, accept(raw));
        }
    }
    void contradictoryCreationRejected() {
        auto root = qEnvironmentVariable("FLORA_PREDICATE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        Frame original((root + "/1/capture.gpa_frame").toStdWString());
        auto a = auditPredicateCreations(original);
        const Id event = a.records.begin()->first;
        const auto c = a.records.begin()->second;
        QTemporaryDir dir;
        for (int mode = 0; mode < 10; mode++) {
            Capture cap;
            for (const auto &[id, e] : original.entries()) {
                auto raw = copy(original.payload(id));
                if (id == event) {
                    if (mode == 0)
                        put(raw, 0, Id(99));
                    if (mode == 1)
                        put(raw, 8, Id(99999));
                    if (mode == 2)
                        put(raw, 16, int32_t(-1));
                    if (mode == 3)
                        put(raw, 16, int32_t(2));
                    if (mode == 4)
                        put(raw, 21, 0u);
                    if (mode == 5)
                        put(raw, 25, 2u);
                    if (mode == 6)
                        put(raw, 29, Id(0));
                    if (mode == 7)
                        put(raw, 29, c.device);
                    if (mode == 8)
                        put(raw, 25, 1u); // valid type/flags but contradicts snapshot
                }
                cap.add(id, e.category, e.type, raw);
            }
            if (mode == 9)
                cap.add(100000, 7, 0x358e, copy(original.payload(event)));
            auto path = dir.filePath(QString("bad-%1.gpa_frame").arg(mode));
            cap.save(path);
            Frame f(path.toStdWString());
            QVERIFY(!auditPredicateCreations(f).records.at(event).error.empty());
            QVERIFY(validateFrame(f.path())["errors"].get<unsigned>() > 0);
            ReplayOptions o;
            o.warp = true;
            Replay r(f, o);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, r.run());
        }
    }
    void futureAndUnissuedRejected() {
        auto root = qEnvironmentVariable("FLORA_PREDICATE_CREATION_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set original corpus");
        Frame original((root + "/1/capture.gpa_frame").toStdWString());
        auto a = auditPredicateCreations(original);
        const Id event = a.records.begin()->first;
        QTemporaryDir dir;
        for (bool future : {false, true}) {
            Capture cap;
            for (const auto &[id, e] : original.entries()) {
                auto op = predicateOperation(e.type);
                if (!future && e.category == 7 &&
                    (op == PredicateOperation::Begin || op == PredicateOperation::End))
                    continue;
                cap.add(future && id == event ? 100000 : id, e.category, e.type, copy(original.payload(id)));
            }
            auto path = dir.filePath(future ? "future.gpa_frame" : "unissued.gpa_frame");
            cap.save(path);
            Frame f(path.toStdWString());
            ReplayOptions o;
            o.warp = true;
            Replay r(f, o);
            bool rejected = false;
            try {
                r.run();
            } catch (const std::exception &e) {
                rejected = std::string(e.what()).find(future ? "before creation" : "no completed captured") !=
                           std::string::npos;
            }
            QVERIFY(rejected);
        }
    }
};
QTEST_GUILESS_MAIN(PredicateCreationTests)
#include "PredicateCreationTests.moc"
