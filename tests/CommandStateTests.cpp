#include "StateCapture.h"
#include "application/CommandState.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
Json field(const Json &result, const char *key) {
    for (const auto &f : result.at("fields"))
        if (f["field"] == key)
            return f;
    throw std::runtime_error(key);
}
void saveJson(const QString &name, const Json &value) {
    auto dir = qEnvironmentVariable("FLORA_STATE_EVIDENCE_DIR");
    if (dir.isEmpty())
        return;
    QDir().mkpath(dir);
    QFile file(dir + '/' + name);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write test evidence");
    file.write(QByteArray::fromStdString(value.dump(2)));
}
} // namespace
class CommandStateTests final : public QObject {
    Q_OBJECT
  private slots:
    void temporalState() {
        QTemporaryDir dir;
        auto c = stateCapture();
        c.save(dir.path() + "/state.gpa_frame");
        Frame frame((dir.path() + "/state.gpa_frame").toStdWString());
        auto read = [&](Id event, bool after = true) { return inspectCommandState(frame, event, after); };
        QCOMPARE(read(100, false)["known_fields"], Json(0));
        QCOMPARE(read(100)["unknown_fields"], Json(0));
        QCOMPARE(field(read(101, false), "vs.shader")["value"], Json(0));
        QCOMPARE(field(read(101), "vs.shader")["value"], Json(50));
        auto dispatch = read(103);
        QCOMPARE(field(dispatch, "vs.shader")["value"], Json(50));
        QCOMPARE(field(dispatch, "vs.shader")["source"]["event"], Json(101));
        QCOMPARE(field(dispatch, "cs.shader")["value"], Json(54));
        QCOMPARE(field(dispatch, "vs.cb_range.0")["value"], Json::array({16, 16}));
        QCOMPARE(field(read(111), "ps.srv.0")["value"], Json(6));
        QCOMPARE(field(read(112), "ps.srv.0")["value"], Json(0));
        QCOMPARE(field(read(112), "ps.srv.0")["source"]["kind"], Json("hazard_null"));
        QCOMPARE(field(read(113), "ps.srv.0")["value"], Json(0));
        QCOMPARE(field(read(115), "ps.srv.0")["value"], Json(6));
        QVERIFY(field(read(116), "ps.srv.0")["value"].is_null());
        QVERIFY(field(read(118), "so.offsets.0")["value"].is_null());
        QCOMPARE(field(read(119), "vb.0")["value"], Json(0));
        QCOMPARE(field(read(120), "vs.shader")["value"], Json(60));
        QVERIFY(field(read(120), "ps.shader")["value"].is_null());
        QCOMPARE(field(read(120), "vs.classes")["value"], Json::array({0x100000001ULL}));
        QCOMPARE(read(121)["known_fields"], Json(0));
        QCOMPARE(read(122)["known_fields"], Json(2));
        QCOMPARE(read(123)["unknown_fields"], Json(0));
        for (Id event : {124, 126, 128, 131, 138}) {
            QCOMPARE(read(event)["known_fields"], Json(0));
            QVERIFY(!read(event)["notes"].empty());
        }
        QCOMPARE(read(130)["unknown_fields"], Json(0));
        QCOMPARE(read(133)["unknown_fields"], Json(0));
        QVERIFY(read(134).contains("unavailable"));
        QVERIFY(read(135).contains("unavailable"));
        QCOMPARE(field(read(137), "blend_factor.0")["value"], Json("nan"));
        QCOMPARE(field(read(137), "blend_factor.1")["value"], Json("inf"));
        auto negative = field(read(137), "blend_factor.3")["value"].get<double>();
        QVERIFY(std::signbit(negative));
        QCOMPARE(field(read(140), "cs.uav.63")["value"], Json(22));
        QCOMPARE(field(read(141), "ps.srv.0")["source"]["kind"], Json("hazard_null"));
        auto bindings = read(156);
        QCOMPARE(field(bindings, "ib")["value"], Json(20));
        QCOMPARE(field(bindings, "hs.cb_range.13")["value"], Json::array({16, 16}));
        QCOMPARE(field(bindings, "hs.srv.127")["value"], Json(6));
        QCOMPARE(field(bindings, "hs.samplers.15")["value"], Json(60));
        QCOMPARE(field(bindings, "predicate_value")["value"], Json(1));
        QCOMPARE(field(bindings, "blend_factor.0")["value"], Json(1.0));
        QCOMPARE(field(bindings, "scissors")["value"], Json::array({Json::array({-2, -3, 4, 5})}));
        QCOMPARE(read(157)["notes"].back()["reason"], Json("Invalid CB1 window granularity or count"));
        QVERIFY(field(read(161), "ps.srv.0")["value"].is_null());
        QCOMPARE(field(read(164), "ps.srv.0")["value"], Json(25));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, read(999));
        auto evidence = qEnvironmentVariable("FLORA_STATE_EVIDENCE_DIR");
        if (!evidence.isEmpty()) {
            QDir().mkpath(evidence);
            c.save(evidence + "/state.gpa_frame");
        }
    }
    void snapshotRangeInvalidation() {
        auto c = stateCapture();
        State state{};
        state.stages[0].cb[0] = 99;
        state.blendFactor[0] = std::numeric_limits<float>::quiet_NaN();
        c.add(91, 3, 3, snapshot(state));
        c.add(165, 7, 0x37, statePack(Id(91), Id(0), Id(1), 3u, 0u));
        QTemporaryDir dir;
        c.save(dir.path() + "/changed.gpa_frame");
        Frame frame((dir.path() + "/changed.gpa_frame").toStdWString());
        auto result = inspectCommandState(frame, 165);
        QVERIFY(field(result, "vs.cb_range.0")["value"].is_null());
        QCOMPARE(field(result, "blend_factor.0")["value"], Json("nan"));
    }
    void allRealSnapshots() {
        auto captures = qEnvironmentVariable("FLORA_TEST_CAPTURE_DIR");
        if (captures.isEmpty())
            QSKIP("External captures not configured");
        for (auto name :
             {"GF2_Exilium_2026_03_03__00_19_35.gpa_frame", "bf1_2026_01_21__16_53_05.gpa_frame"}) {
            Frame frame((captures + '/' + name).toStdWString());
            bool gf = QString(name).startsWith("GF2");
            auto audit = auditCommandState(frame);
            saveJson(gf ? "gf2-audit.json" : "bf1-audit.json", audit);
            QCOMPARE(audit["checked"], Json(gf ? 86380 : 1545910));
            QVERIFY(audit["differences"].empty());
            Json events = Json::array();
            for (const auto &note : audit["notes"])
                events.push_back(note["event"]);
            QCOMPARE(events, gf ? Json::array() : Json::array({25572, 27683, 27692, 27696}));
            if (!gf) {
                auto result = inspectCommandState(frame, 20434, true);
                QCOMPARE(field(result, "vs.shader")["value"], Json(20396));
                QCOMPARE(field(result, "ps.shader")["value"], Json(20398));
                QCOMPARE(field(result, "cs.cb_range.0")["value"], Json::array({2736, 16}));
            }
        }
    }
};
QTEST_GUILESS_MAIN(CommandStateTests)
#include "CommandStateTests.moc"
