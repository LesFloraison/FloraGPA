#include "SrvBindingCapture.h"
#include "StateCapture.h"
#include "application/Experiment.h"
#include "application/SetterEdits.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class SrvBindingTests final : public QObject {
    Q_OBJECT
  private slots:
    void validation() {
        QTemporaryDir dir;
        srvBindingCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        QCOMPARE(capturedSetter(f, 910), Json({{"start_slot", 0}, {"views", {732}}}));
        for (auto j : std::vector<Json>{{{"start_slot", true}, {"views", {0}}},
                                        {{"start_slot", 128}, {"views", Json::array()}},
                                        {{"start_slot", 127}, {"views", {0, 0}}},
                                        {{"start_slot", 0}, {"views", {730}}},
                                        {{"start_slot", 0}, {"views", {-1}}},
                                        {{"start_slot", 0}, {"views", {999999}}}})
            QVERIFY_THROWS_EXCEPTION(std::exception, validateSrvSetter(f, 910, j));
        auto bytes = statePack(Id(0), Id(1), 0u, 1u, uint8_t(0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readSrvCommand(bytes));
        auto noOp = readSrvCommand(statePack(Id(0), Id(1), 127u, 0u, uint8_t(0)));
        QVERIFY(noOp.binding.views.empty());
        Experiment e(f);
        e.setSetter(f, 910, {{"start_slot", 0}, {"views", {733}}});
        QCOMPARE(e.srv(f, 1000, "ps", 0)["most_detailed_mip"], Json(1));
        e.setSrv(f, 1000, "ps", 0, {{"mip_levels", 1}});
        auto old = e.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 e.setSetter(f, 910, {{"start_slot", 0}, {"views", {0}}}));
        QCOMPARE(e.document(), old);
        e.save(dir.path() + "/experiment.json");
        Experiment loaded(f);
        loaded.load(dir.path() + "/experiment.json", f);
        QCOMPARE(loaded.document(), old);
        QVERIFY(loaded.undo());
        QVERIFY(loaded.undo());
        QCOMPARE(loaded.srv(f, 1000, "ps", 0)["most_detailed_mip"], Json(0));
        QVERIFY(loaded.redo());
        QCOMPARE(loaded.srv(f, 1000, "ps", 0)["most_detailed_mip"], Json(1));
    }
    void history() {
        QTemporaryDir dir;
        auto c = srvBindingCapture();
        c.add(1500, 7, 0x34e6, statePack(Id(0), Id(1), 1u, 1u, uint8_t(1), Id(0)));
        c.add(2100, 7, 0x242, statePack(Id(0), Id(1), uint8_t(1)));
        c.save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        SrvHistory h(f, 1);
        QVERIFY(!h.advance(840).srvs[4][0]);
        QCOMPARE(h.advance(840, true).srvs[4][0], std::optional<Id>(732));
        SrvBindings tracker;
        SrvBinding original{0, {0}}, edited{1, {733}};
        tracker.transition(4, original, &edited, h.advance(910));
        State s{};
        tracker.apply(s);
        QCOMPARE(s.stages[4].srv[0], Id(732));
        QCOMPARE(s.stages[4].srv[1], Id(733));
        std::map<Id, SrvBinding> edits{{910, edited}};
        auto state = effectiveSrvBindings(f, 1000, f.state(990), edits);
        QCOMPARE(state.stages[4].srv[0], Id(732));
        QCOMPARE(state.stages[4].srv[1], Id(733));
        QCOMPARE(effectiveSrvBindings(f, 2000, f.state(1990), edits).stages[4].srv[1], Id(0));
        QCOMPARE(effectiveSrvBindings(f, 1000, f.state(990), edits).stages[4].srv[1], Id(733));
        tracker.clear();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 tracker.transition(4, original, &edited, SrvObservation{}));
        QVERIFY(!tracker.active());
        QVERIFY(!h.advance(2100, true).srvs[4][0]);
    }
    void contextRecordOrder() {
        QTemporaryDir dir;
        auto c = srvBindingCapture();
        c.add(780, 5, 0x127, std::vector<uint8_t>(24));
        for (auto &entry : c.entries)
            if (entry.id == 910)
                put(c.bytes, size_t(entry.offset) + 8, Id(780));
        c.save(dir.path() + "/contexts.gpa_frame");
        Frame f((dir.path() + "/contexts.gpa_frame").toStdWString());
        Experiment e(f);
        e.setSetter(f, 910, {{"start_slot", 0}, {"views", {733}}});
        QCOMPARE(e.srv(f, 1000, "ps", 0)["most_detailed_mip"], Json(1));
        e.setSrv(f, 1000, "ps", 0, {{"mip_levels", 1}});
        ReplayOptions options;
        e.apply(f, options);
        QCOMPARE(options.srvEdits.at(1000).at({4, 0}).Texture2D.MostDetailedMip, 1u);
    }
    void bufferHistory() {
        QTemporaryDir dir;
        auto c = computeCapture();
        c.buffer(34, 35, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, {20, 21, 22, 23});
        c.add(36, 5, 0x8c, statePack(Id(0), Id(0), Id(34), 0u, 1u, 0u, 4u, 0u, 0u));
        c.add(95, 7, 0x3521, statePack(Id(0), Id(1), 0u, 1u, uint8_t(1), Id(6)));
        c.save(dir.path() + "/buffer.gpa_frame");
        Frame f((dir.path() + "/buffer.gpa_frame").toStdWString());
        Experiment e(f);
        const auto data = statePack(42u);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, e.setBuffer(f, 100, 34, 0, data));
        QCOMPARE(e.revision(), size_t(0));
        e.setSetter(f, 95, {{"start_slot", 0}, {"views", {36}}});
        e.setBuffer(f, 100, 34, 0, data);
        auto saved = e.document();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, e.setSetter(f, 95, {{"start_slot", 0}, {"views", {6}}}));
        QCOMPARE(e.document(), saved);
        e.save(dir.path() + "/project.json");
        Experiment loaded(f);
        loaded.load(dir.path() + "/project.json", f);
        QCOMPARE(loaded.document(), saved);
        ReplayOptions options;
        loaded.apply(f, options);
        QCOMPARE(options.buffers.at(100).at(34).at(0).bytes, data);
        QVERIFY(loaded.undo());
        loaded.setSetter(f, 95, {{"start_slot", 0}, {"views", {6}}});
        QCOMPARE(loaded.srv(f, 100, "cs", 0)["num_elements"], Json(4));
    }
    void hazards() {
        QTemporaryDir dir;
        auto c = stateCapture();
        c.add(300, 5, 0x85, statePack(Id(0), Id(0), 16u, 16u, 1u, 1u, 44u, 1u, 0u, 0u, 72u, 0u, 0u, Id(0)));
        c.add(303, 5, 0x8c, statePack(Id(0), Id(0), Id(300), 46u, 4u, 0u, 1u, 0u, 0u));
        c.add(304, 5, 0x8c, statePack(Id(0), Id(0), Id(300), 47u, 4u, 0u, 1u, 0u, 0u));
        for (unsigned flag = 0; flag < 4; ++flag)
            c.add(305 + flag, 5, 0x8e, statePack(Id(0), Id(0), Id(300), 45u, 3u, flag, 0u, 0u, 0u));
        c.save(dir.path() + "/hazards.gpa_frame");
        Frame f((dir.path() + "/hazards.gpa_frame").toStdWString());
        SrvHazards check(f);
        QCOMPARE(check.overlap(6, 7, true), std::optional<bool>(false));
        QCOMPARE(check.overlap(6, 8, true), std::optional<bool>(true));
        QCOMPARE(check.overlap(25, 26, true), std::optional<bool>(true));
        QCOMPARE(check.overlap(25, 27, true), std::optional<bool>(false));
        QVERIFY(!check.overlap(25, 26).has_value());
        for (unsigned flags = 0; flags < 4; ++flags) {
            QCOMPARE(check.overlap(303, 305 + flags, true), std::optional<bool>(!(flags & 1)));
            QCOMPARE(check.overlap(304, 305 + flags, true), std::optional<bool>(!(flags & 2)));
        }
        SrvOutputs outputs{};
        QVERIFY(!check.effective(303, outputs, true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, check.effective(303, outputs));
        outputs.fill(Id(0));
        outputs[140] = 306;
        QCOMPARE(check.effective(303, outputs), std::optional<Id>(303));
        QCOMPARE(check.effective(304, outputs), std::optional<Id>(0));
        outputs[140] = 999999;
        QVERIFY(!check.effective(303, outputs, true));
        QVERIFY(!check.effective(999999, outputs, true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, check.effective(999999, outputs));
    }
    void gpu_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void gpu() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        srvBindingCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment e(f);
        e.setSetter(f, 910, {{"start_slot", 0}, {"views", {733}}});
        for (Id event : {Id(1000), Id(2000)}) {
            ReplayOptions o;
            o.warp = warp;
            o.until = event;
            e.apply(f, o);
            Replay r(f, o);
            r.run();
            QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{191, 0, 0, 255}));
            r.run();
            QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{191, 0, 0, 255}));
        }
        e.setSetter(f, 910, {{"start_slot", 127}, {"views", {733}}});
        ReplayOptions o;
        o.warp = warp;
        o.until = 1000;
        e.apply(f, o);
        Replay r(f, o);
        r.run();
        QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{32, 0, 0, 255}));
        r.inspectNativeState([&](auto *ctx, const auto &) {
            Com<ID3D11ShaderResourceView> view;
            ctx->PSGetShaderResources(127, 1, &view);
            QVERIFY(view);
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            view->GetDesc(&desc);
            QCOMPARE(desc.Texture2D.MostDetailedMip, 1u);
        });
    }
};
QTEST_GUILESS_MAIN(SrvBindingTests)
#include "SrvBindingTests.moc"
