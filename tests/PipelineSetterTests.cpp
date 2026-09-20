#include "PipelineSetterCapture.h"
#include "application/Experiment.h"
#include "application/SetterEdits.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <fstream>
#include <iostream>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
class PipelineSetterTests : public QObject {
    Q_OBJECT
  private slots:
    void persistentAndReset() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/pipeline.gpa_frame";
        pipelineSetterCapture().save(path);
        Frame frame(path.toStdWString());
        Experiment project(frame);
        const std::map<Id, Json> edits{
            {90, {{"topology", 1}}},
            {91, {{"blend", 0}, {"blend_factor", {.25, .5, .75, 1.}}, {"sample_mask", 3}}},
            {92, {{"depth_state", 0}, {"stencil_ref", 45}}},
            {93, {{"rasterizer", 0}}},
            {94, {{"viewports_values", {{1., 1., 2., 3., 0., 1.}}}}},
            {95, {{"scissors_values", {{1, 1, 3, 4}}}}}};
        for (auto &[id, values] : edits)
            project.setSetter(frame, id, values);
        ReplayOptions options;
        options.warp = true;
        options.before = true;
        options.until = 100;
        project.apply(frame, options);
        const auto state = effectiveBindings(frame, 100, frame.state(frame.event(100).state), options);
        QCOMPARE(state.topology, 1u);
        QVERIFY(state.viewportValues);
        QCOMPARE(state.viewportValues->at(0)[2], 2.f);
        QCOMPARE(project.rasterizer(frame, 100)["viewports"][0][2], Json(2.));
        QCOMPARE(project.blend(frame, 100)["blend_factor"][0], Json(.25));
        QCOMPARE(project.depthStencil(frame, 100)["stencil_ref"], Json(45));
        Replay replay(frame, options);
        replay.run();
        replay.inspectNativeState([&](auto *ctx, const auto &) {
            D3D11_PRIMITIVE_TOPOLOGY topology;
            ctx->IAGetPrimitiveTopology(&topology);
            QCOMPARE(uint32_t(topology), 1u);
            UINT count = 16;
            D3D11_VIEWPORT rows[16];
            ctx->RSGetViewports(&count, rows);
            QCOMPARE(count, 1u);
            QCOMPARE(rows[0].Width, 2.f);
            float factor[4];
            UINT mask;
            ctx->OMGetBlendState(nullptr, factor, &mask);
            QCOMPARE(factor[0], .25f);
            QCOMPARE(mask, 3u);
            UINT reference;
            ctx->OMGetDepthStencilState(nullptr, &reference);
            QCOMPARE(reference, 45u);
        });
        options.until = 105;
        options.before = false;
        Replay reset(frame, options);
        reset.run();
        reset.inspectNativeState([&](auto *ctx, const auto &) {
            D3D11_PRIMITIVE_TOPOLOGY topology;
            ctx->IAGetPrimitiveTopology(&topology);
            QCOMPARE(uint32_t(topology), 4u);
        });
        options.until = 110;
        options.before = true;
        Replay cleared(frame, options);
        cleared.run();
        cleared.inspectNativeState([&](auto *ctx, const auto &) {
            UINT n = 1;
            D3D11_VIEWPORT v;
            ctx->RSGetViewports(&n, &v);
            QCOMPARE(n, 1u);
            QCOMPARE(v.Width, 7.f);
        });
        project.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.setter(frame, 94), project.setter(frame, 94));
        loaded.undo();
        loaded.redo();
        QCOMPARE(loaded.document(), project.document());
        const auto before = project.document();
        QVERIFY_THROWS_EXCEPTION(std::exception, project.setSetter(frame, 90, {{"topology", 6}}));
        QCOMPARE(project.document(), before);
    }
    void inheritedPipeline() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/inherit.gpa_frame";
        pipelineSetterCapture().save(path);
        Frame frame(path.toStdWString());
        Experiment project(frame);
        // Per-draw edit first, persistent setter later: final history order must not change inheritance.
        project.setBlend(frame, 100, {{"blend_state", {{"alpha_to_coverage", true}}}});
        project.setRasterizer(frame, 100, {{"rasterizer", {{"depth_bias", 7}}}});
        project.setSetter(frame, 91,
                          {{"blend", 0}, {"blend_factor", {.125, .25, .5, 1.}}, {"sample_mask", 7}});
        project.setSetter(frame, 93, {{"rasterizer", 0}});
        ReplayOptions options;
        project.apply(frame, options);
        QCOMPARE(options.rasterizerEdits.at(100).descriptor->CullMode, D3D11_CULL_BACK);
        QCOMPARE(project.blend(frame, 100)["sample_mask"], Json(7));
        QCOMPARE(project.rasterizer(frame, 100)["rasterizer"]["cull_mode"], Json(3));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--oracle") {
        std::ifstream input(argv[2]);
        Json request;
        input >> request;
        Json results = Json::array();
        for (const auto &op : request.at("cases"))
            try {
                Frame frame(QString::fromStdString(op.at("path").get<std::string>()).toStdWString());
                auto id = op.at("event").get<Id>();
                auto value = op.contains("values")
                                 ? pipelineSetterValues(validatePipelineSetter(frame, id, op.at("values")))
                                 : capturedSetter(frame, id);
                results.push_back({{"status", "ok"}, {"value", value}});
            } catch (const std::exception &e) {
                results.push_back({{"status", "error"}, {"error", e.what()}});
            }
        std::cout << results.dump();
        return 0;
    }
    PipelineSetterTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "PipelineSetterTests.moc"
