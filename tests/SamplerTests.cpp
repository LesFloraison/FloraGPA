#include "SamplerCapture.h"
#include "application/Experiment.h"
#include "application/SamplerEdits.h"
#include "application/SetterEdits.h"
#include <QTemporaryDir>
#include <QtTest>
#include <limits>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class SamplerTests : public QObject {
    Q_OBJECT
  private slots:
    void wireAndInvalid() {
        const auto wire = statePack(Id(0), Id(1), 15u, 1u, uint8_t(1), Id(0x100000001));
        QCOMPARE(readSamplerCommand(wire).binding.resources[0], Id(0x100000001));
        for (size_t i = 0; i < wire.size(); ++i)
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSamplerCommand(Bytes(wire.data(), i)));
        auto extra = wire;
        extra.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSamplerCommand(extra));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 readSamplerCommand(statePack(Id(0), Id(1), 0u, 1u, uint8_t(0))));
        for (auto value : std::vector<Json>{Json::object(),
                                            {{"filter", true}},
                                            {{"filter", 2}},
                                            {{"address_u", 0}},
                                            {{"address_w", 6}},
                                            {{"filter", 85}, {"max_anisotropy", 0}},
                                            {{"max_anisotropy", 17}},
                                            {{"comparison_func", 9}},
                                            {{"border_color", {0, 0, 0}}},
                                            {{"border_color", {0, 0, 0, 2}}},
                                            {{"mip_lod_bias", 16}},
                                            {{"min_lod", std::numeric_limits<double>::infinity()}},
                                            {{"min_lod", 2}, {"max_lod", 1}},
                                            {{"unknown", 0}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizeSampler(value));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, samplerStage("pixel"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, samplerSlot(true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, samplerSlot(16));
    }
    void rangeHistory() {
        for (unsigned stage = 0; stage < 6; ++stage) {
            SamplerBindings tracker;
            SamplerBinding original{2, {0, 0}}, replacement{3, {741}};
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, tracker.transition(stage, original, &replacement));
            tracker.observeCommand(stage, {2, {740, 0}});
            tracker.transition(stage, original, &replacement);
            tracker.observeCommand(stage, original);
            State s{};
            tracker.apply(s);
            QCOMPARE(s.stages[stage].samplers[2], Id(740));
            QCOMPARE(s.stages[stage].samplers[3], Id(741));
            tracker.observe(State{});
            s = {};
            tracker.apply(s);
            QCOMPARE(s.stages[stage].samplers[2], Id(740));
            tracker.transition(stage, {3, {0}}, nullptr);
            s = {};
            tracker.apply(s);
            QCOMPARE(s.stages[stage].samplers[2], Id(740));
            QCOMPARE(s.stages[stage].samplers[3], Id(0));
            tracker.transition(stage, {2, {}}, nullptr);
            tracker.transition(stage, {9, {0}}, nullptr);
            s = {};
            tracker.apply(s);
            QCOMPARE(s.stages[stage].samplers[2], Id(740));
            tracker.clear();
            s = {};
            tracker.apply(s);
            QCOMPARE(s.stages[stage].samplers[2], Id(0));
        }
    }
    void historyAndInheritance() {
        QTemporaryDir dir;
        samplerCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        for (bool reverse : {false, true}) {
            Experiment e(f);
            auto setter = [&] { e.setSetter(f, 900, {{"start_slot", 2}, {"samplers", {741}}}); };
            auto descriptor = [&] { e.setSampler(f, 1000, "ps", 2, {{"border_color", {.25, 0, 0, 1}}}); };
            if (reverse) {
                descriptor();
                setter();
            } else {
                setter();
                descriptor();
            }
            QCOMPARE(e.sampler(f, 1000, "ps", 2)["address_u"], Json(4));
            QCOMPARE(e.sampler(f, 1000, "ps", 2)["border_color"], Json({.25, 0, 0, 1}));
            auto old = e.document();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, e.setSampler(f, 1000, "ps", 2, {{"min_lod", 1}}));
            QCOMPARE(e.document(), old);
            auto file = dir.path() + "/project.json";
            e.save(file);
            Experiment loaded(f);
            loaded.load(file, f);
            QCOMPARE(loaded.document(), e.document());
            QVERIFY(loaded.undo());
            QVERIFY(loaded.redo());
            QCOMPARE(loaded.document(), e.document());
        }
        Experiment e(f);
        e.setSampler(f, 1000, "ps", 2, {{"min_lod", 1}});
        const auto prior = e.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 e.setSetter(f, 900, {{"start_slot", 2}, {"samplers", {741}}}));
        QCOMPARE(e.document(), prior);
        QVERIFY(e.undo());
        e.setSetter(f, 900, {{"start_slot", 3}, {"samplers", Json::array()}});
        ReplayOptions o;
        e.apply(f, o);
        QCOMPARE(samplerState(f, 1000, o.samplerSetters).stages[4].samplers[2], Id(740));
        samplerState(f, 2000, o.samplerSetters);
        QCOMPARE(samplerState(f, 1000, o.samplerSetters).stages[4].samplers[2], Id(740));
        for (auto v : std::vector<Json>{{{"start_slot", true}, {"samplers", {740}}},
                                        {{"start_slot", 16}, {"samplers", Json::array()}},
                                        {{"start_slot", 15}, {"samplers", {740, 741}}},
                                        {{"start_slot", 0}, {"samplers", {true}}},
                                        {{"start_slot", 0}, {"samplers", {732}}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, e.setSetter(f, 900, v));
    }
    void prefixBoundaries() {
        QTemporaryDir dir;
        auto c = samplerCapture();
        c.add(1050, 7, 0x34e8, statePack(Id(0), Id(1), 15u, 1u, uint8_t(0)));
        c.save(dir.path() + "/future.gpa_frame");
        Frame f((dir.path() + "/future.gpa_frame").toStdWString());
        std::map<Id, SamplerBinding> edits{{900, {3, {741}}}};
        QCOMPARE(samplerState(f, 1000, edits).stages[4].samplers[2], Id(740));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, samplerState(f, 2000, edits));
        QCOMPARE(samplerState(f, 1000, edits).stages[4].samplers[2], Id(740));
        c.entries.erase(
            std::remove_if(c.entries.begin(), c.entries.end(), [](const auto &e) { return e.id == 850; }),
            c.entries.end());
        c.save(dir.path() + "/unknown.gpa_frame");
        Frame unknown((dir.path() + "/unknown.gpa_frame").toStdWString());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, samplerState(unknown, 1000, edits));
        // A malformed command in an inactive stage does not invalidate offline
        // descriptor inheritance when no displaced prefix observation is needed.
        c.add(875, 7, 0x34f8, statePack(Id(0), Id(1), 15u, 1u, uint8_t(0)));
        c.save(dir.path() + "/inactive.gpa_frame");
        Frame inactive((dir.path() + "/inactive.gpa_frame").toStdWString());
        edits[900] = {2, {741}};
        QCOMPARE(samplerState(inactive, 1000, edits).stages[4].samplers[2], Id(741));
        edits[900] = {3, {741}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, samplerState(inactive, 1000, edits));
    }
    void gpu_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void gpu() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        samplerCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment e(f);
        e.setSetter(f, 900, {{"start_slot", 2}, {"samplers", {741}}});
        e.setSampler(f, 1000, "ps", 2, {{"border_color", {.25, 0, 0, 1}}});
        auto run = [&](Id event, bool before = false) {
            ReplayOptions o;
            o.warp = warp;
            o.until = event;
            o.before = before;
            e.apply(f, o);
            Replay r(f, o);
            r.run();
            auto pixels = r.output(20).rgba;
            r.inspectNativeState([&](auto *context, const auto &) {
                Com<ID3D11SamplerState> sampler;
                context->PSGetSamplers(2, 1, &sampler);
                QVERIFY(sampler);
                D3D11_SAMPLER_DESC d{};
                sampler->GetDesc(&d);
                QCOMPARE(d.BorderColor[0], event == 1000 ? .25f : 0.f);
                QCOMPARE(d.BorderColor[1], event == 1000 ? 0.f : .375f);
            });
            r.run();
            QCOMPARE(r.output(20).rgba, pixels);
            const std::vector<uint8_t> expected = before          ? std::vector<uint8_t>{0, 0, 0, 255}
                                                  : event == 1000 ? std::vector<uint8_t>{64, 0, 0, 255}
                                                                  : std::vector<uint8_t>{0, 96, 0, 255};
            QCOMPARE(pixels, expected);
        };
        run(1000);
        run(1000, true);
        run(2000);
        QVERIFY(e.undo());
        ReplayOptions o;
        o.warp = warp;
        o.until = 1000;
        e.apply(f, o);
        Replay r(f, o);
        r.run();
        QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{0, 96, 0, 255}));
        QVERIFY(e.redo());
        run(1000);
    }
};
QTEST_GUILESS_MAIN(SamplerTests)
#include "SamplerTests.moc"
