#include "SrvCapture.h"
#include "application/Experiment.h"
#include "application/SrvEdits.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class SrvTests : public QObject {
    Q_OBJECT
  private slots:
    void descriptors() {
        for (unsigned dim = 1; dim <= 11; ++dim) {
            Json v{{"format", 41}, {"dimension", dim}};
            for (const auto &name : srvFields(dim))
                v[name] = 1;
            auto desc = nativeSrv(v);
            QCOMPARE(unsigned(desc.Format), 41u);
            QCOMPARE(unsigned(desc.ViewDimension), dim);
            std::array<uint32_t, 6> words;
            std::memcpy(words.data(), &desc, sizeof desc);
            for (size_t i = 2; i < 6; ++i)
                QCOMPARE(words[i], i < srvFields(dim).size() + 2 ? 1u : 0u);
        }
        auto merged = mergeSrv({{"format", 41},
                                {"dimension", 5},
                                {"first_array_slice", 2},
                                {"array_size", 1},
                                {"most_detailed_mip", 0},
                                {"mip_levels", 1}},
                               {{"dimension", 4}});
        QVERIFY(!merged.contains("array_size"));
        QVERIFY(!merged.contains("first_array_slice"));
        nativeSrv(merged);
        for (auto value : std::vector<Json>{Json::object(),
                                            {{"format", true}},
                                            {{"unknown", 1}},
                                            {{"dimension", 0}},
                                            {{"dimension", 12}},
                                            {{"first_element", -1}},
                                            {{"mip_levels", 0}},
                                            {{"flags", 2}},
                                            {{"num_cubes", 0}},
                                            {{"array_size", uint64_t(1) << 32}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizeSrv(value));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, srvSlot(true));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, srvSlot(128));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, srvStage("bad"));
    }
    void history() {
        QTemporaryDir dir;
        srvCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment e(f);
        e.setSrv(f, 1000, "ps", 0, {{"most_detailed_mip", 1}, {"mip_levels", 1}});
        QCOMPARE(e.srv(f, 1000, "ps", 0)["most_detailed_mip"], Json(1));
        auto old = e.document();
        for (auto value : std::vector<Json>{{{"first_element", 1}}, {{"dimension", 5}}}) {
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, e.setSrv(f, 1000, "ps", 0, value));
            QCOMPARE(e.document(), old);
        }
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, e.setSrv(f, 1000, "ps", 1, {{"format", 2}}));
        QCOMPARE(e.document(), old);
        auto file = dir.path() + "/project.json";
        e.save(file);
        Experiment loaded(f);
        loaded.load(file, f);
        QCOMPARE(loaded.document(), old);
        QVERIFY(loaded.undo());
        QCOMPARE(loaded.srv(f, 1000, "ps", 0)["most_detailed_mip"], Json(0));
        QVERIFY(loaded.redo());
        QCOMPARE(loaded.document(), old);
        e.setSrv(f, 1000, "ps", 0, {{"dimension", 5}, {"first_array_slice", 0}, {"array_size", 1}});
        e.setSrv(f, 1000, "ps", 0, {{"dimension", 4}});
        QCOMPARE(e.srv(f, 1000, "ps", 0)["dimension"], Json(4));
    }
    void gpu_data() {
        QTest::addColumn<bool>("warp");
        QTest::newRow("hardware") << false;
        QTest::newRow("warp") << true;
    }
    void gpu() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        srvCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment e(f);
        e.setSrv(f, 1000, "ps", 0, {{"most_detailed_mip", 1}, {"mip_levels", 1}});
        for (Id event : {Id(1000), Id(2000)})
            for (bool before : {false, true}) {
                ReplayOptions o;
                o.warp = warp;
                o.until = event;
                o.before = before;
                e.apply(f, o);
                Replay r(f, o);
                r.run();
                r.inspectNativeState([&](auto *ctx, const auto &objects) {
                    Com<ID3D11ShaderResourceView> view;
                    ctx->PSGetShaderResources(0, 1, &view);
                    QVERIFY(view);
                    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
                    view->GetDesc(&d);
                    QCOMPARE(d.Texture2D.MostDetailedMip, event == 1000 ? 1u : 0u);
                    if (event == 1000)
                        QVERIFY(view.Get() != objects.at(732).Get());
                });
                auto pixels = r.output(20).rgba;
                r.run();
                QCOMPARE(r.output(20).rgba, pixels);
                const auto expected = before && event == 1000   ? std::vector<uint8_t>{0, 0, 0, 255}
                                      : event == 1000 || before ? std::vector<uint8_t>{191, 0, 0, 255}
                                                                : std::vector<uint8_t>{32, 0, 0, 255};
                QCOMPARE(pixels, expected);
            }
        QVERIFY(e.undo());
        ReplayOptions o;
        o.warp = warp;
        o.until = 1000;
        e.apply(f, o);
        Replay r(f, o);
        r.run();
        QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{32, 0, 0, 255}));
        e.setSrv(f, 1000, "ps", 0, {{"most_detailed_mip", 2}});
        e.apply(f, o);
        Replay bad(f, o);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bad.run());
    }
    void clone_data() { gpu_data(); }
    void clone() {
        QFETCH(bool, warp);
        QTemporaryDir dir;
        computeCapture().save(dir.path() + "/frame.gpa_frame");
        Frame f((dir.path() + "/frame.gpa_frame").toStdWString());
        Experiment e(f);
        auto patch = word(42);
        e.setBuffer(f, 100, 4, 4, patch);
        e.setSrv(f, 100, "cs", 0, {{"first_element", 1}, {"num_elements", 3}});
        ReplayOptions o;
        o.warp = warp;
        o.until = 100;
        e.apply(f, o);
        Replay r(f, o);
        r.run();
        QCOMPARE(firstWord(r, 7), 54u);
        const auto original = statePack(7u, 8u, 9u, 10u);
        QCOMPARE(r.readBuffer(4), original);
        r.inspectNativeState([&](auto *ctx, const auto &objects) {
            Com<ID3D11ShaderResourceView> view;
            ctx->CSGetShaderResources(0, 1, &view);
            QVERIFY(view);
            Com<ID3D11Resource> owner;
            view->GetResource(&owner);
            QVERIFY(owner.Get() != objects.at(4).Get());
            D3D11_SHADER_RESOURCE_VIEW_DESC d{};
            view->GetDesc(&d);
            QCOMPARE(d.Buffer.FirstElement, 1u);
        });
        auto outputPatch = word(99);
        e.setBuffer(f, 100, 7, 0, outputPatch);
        e.setSrv(f, 100, "cs", 0, {{"format", 28}});
        e.apply(f, o);
        Replay failed(f, o);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, failed.run());
        QCOMPARE(failed.counts["Dispatch"], uint64_t(0));
        QCOMPARE(firstWord(failed, 7), 10u);
        QCOMPARE(failed.readBuffer(4), original);
    }
};
QTEST_GUILESS_MAIN(SrvTests)
#include "SrvTests.moc"
