#include "DepthStencilCapture.h"
#include "application/Experiment.h"
#include "application/RasterizerEdits.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
class RasterizerTests final : public QObject {
    Q_OBJECT
  private slots:
    void historyAndBoundaries() {
        QTemporaryDir dir;
        testing::depthStencilCapture().save(dir.path() + "/rasterizer.gpa_frame");
        Frame frame((dir.path() + "/rasterizer.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setRasterizer(frame, 1000,
                              {{"rasterizer", {{"cull_mode", 3}, {"depth_bias", 7}}}, {"wireframe", true}});
        project.setRasterizer(frame, 1000,
                              {{"cull_none", false}, {"wireframe", false}, {"depth_test", false}});
        project.setRasterizer(
            frame, 1000,
            {{"rasterizer", {{"scissor_enable", true}}}, {"scissors", Json::array({{0, 0, 0, 0}})}});
        auto effective = project.rasterizer(frame, 1000);
        QCOMPARE(effective["rasterizer"]["cull_mode"], Json(1));
        QCOMPARE(effective["rasterizer"]["depth_bias"], Json(7));
        QCOMPARE(project.depthStencil(frame, 1000)["depth_stencil"]["depth_enable"], Json(false));
        project.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.document(), project.document());
        for (bool warp : {false, true})
            for (auto boundary : {std::pair<Id, bool>{1000, false}, {1000, true}, {2000, false}}) {
                ReplayOptions options;
                options.warp = warp;
                options.until = boundary.first;
                options.before = boundary.second;
                loaded.apply(frame, options);
                Replay replay(frame, options);
                for (int repeat = 0; repeat < 2; ++repeat) {
                    replay.run();
                    replay.inspectNativeState([&](auto *context, const auto &) {
                        Com<ID3D11RasterizerState> state;
                        context->RSGetState(&state);
                        QVERIFY(state);
                        D3D11_RASTERIZER_DESC d{};
                        state->GetDesc(&d);
                        QCOMPARE(d.FillMode, D3D11_FILL_SOLID);
                        QCOMPARE(d.CullMode, D3D11_CULL_NONE);
                        QCOMPARE(d.DepthBias, boundary.first == 1000 ? 7 : 0);
                        QCOMPARE(bool(d.ScissorEnable), boundary.first == 1000);
                    });
                    QCOMPARE(replay.output(20).rgba, boundary.first == 1000
                                                         ? (std::vector<uint8_t>{0, 0, 0, 255})
                                                         : (std::vector<uint8_t>{255, 0, 0, 255}));
                }
            }
        QVERIFY(project.undo());
        QCOMPARE(project.rasterizer(frame, 1000)["rasterizer"]["scissor_enable"], Json(false));
        QVERIFY(project.redo());
        QCOMPARE(project.rasterizer(frame, 1000), effective);
        QVERIFY(project.undo());
        const auto previous = project.document();
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            project.setRasterizer(frame, 1000,
                                  {{"wireframe", true}, {"rasterizer", {{"conservative_raster", 1}}}}));
        QCOMPARE(project.document(), previous);
        QVERIFY(project.redo());
        QVERIFY(project.undo());
        project.setRasterizer(frame, 1000, {{"viewports", Json::array()}});
        QVERIFY(!project.redo());
        ReplayOptions options;
        options.warp = true;
        options.until = 1000;
        project.apply(frame, options);
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.output(20).rgba, (std::vector<uint8_t>{0, 0, 0, 255}));
        replay.inspectNativeState([&](auto *context, const auto &) {
            UINT count = 16;
            D3D11_VIEWPORT viewports[16]{};
            context->RSGetViewports(&count, viewports);
            QCOMPARE(count, 0u);
        });
    }
    void arrayBoundsAndTypes() {
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 normalizePipeline({{"viewports", {{32767, 0, 1, 1, 0, 1}}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizePipeline({{"scissors", {{0, 0, true, 1}}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 normalizePipeline({{"rasterizer", {{"depth_bias", UINT64_MAX}}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 normalizePipeline({{"rasterizer", {{"forced_sample_count", 3}}}}));
        auto rows = Json::array();
        for (int i = 0; i < 16; ++i)
            rows.push_back({-32768, -32768, 65535, 65535, 0, 1});
        QCOMPARE(normalizePipeline({{"viewports", rows}})["viewports"].size(), size_t(16));
        rows.push_back(rows[0]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizePipeline({{"viewports", rows}}));
    }
};
QTEST_GUILESS_MAIN(RasterizerTests)
#include "RasterizerTests.moc"
