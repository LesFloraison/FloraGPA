#include "DepthStencilCapture.h"
#include "application/DepthStencilEdits.h"
#include "application/Experiment.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
class DepthStencilTests final : public QObject {
    Q_OBJECT
    uint32_t storedDepth(Replay &replay) {
        Com<ID3D11DeviceContext> context;
        Com<ID3D11Texture2D> source;
        replay.inspectNativeState([&](auto *native, const auto &objects) {
            context = native;
            check(objects.at(720).As(&source), "Get depth storage");
        });
        Com<ID3D11Device> device;
        context->GetDevice(&device);
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Depth test staging");
        context->CopyResource(staging.Get(), source.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read depth test storage");
        uint32_t word;
        std::memcpy(&word, mapped.pData, sizeof word);
        context->Unmap(staging.Get(), 0);
        return word;
    }
  private slots:
    void stencilStorage() {
        const uint32_t expected[]{0, 0x53, 0, 0xa5, 0x54, 0x52, 0xac, 0x54, 0x52};
        for (bool back : {false, true}) {
            auto c = testing::depthStencilCapture(back);
            QTemporaryDir dir;
            c.save(dir.path() + "/stencil.gpa_frame");
            Frame frame((dir.path() + "/stencil.gpa_frame").toStdWString());
            for (bool warp : {false, true})
                for (const auto branch : {"fail_op", "depth_fail_op", "pass_op"})
                    for (uint32_t op = 1; op <= 8; ++op) {
                        Experiment project(frame);
                        nlohmann::json ds{
                            {"stencil_enable", true},
                            {"depth_write_mask", 0},
                            {"depth_func", std::string(branch) == "depth_fail_op" ? 1 : 8},
                            {back ? "back_face" : "front_face",
                             {{"func", std::string(branch) == "fail_op" ? 1 : 8}, {branch, op}}}};
                        project.setDepthStencil(frame, 1000, {{"depth_stencil", ds}, {"stencil_ref", 0xa5}});
                        ReplayOptions o;
                        o.warp = warp;
                        o.until = 1000;
                        project.apply(frame, o);
                        Replay replay(frame, o);
                        replay.run();
                        QCOMPARE(storedDepth(replay) >> 24, expected[op]);
                        QVERIFY((storedDepth(replay) & 0xffffff) >= 0x7fffffu);
                    }
        }
    }
    void historyAndReplay() {
        auto c = testing::depthStencilCapture();
        QTemporaryDir dir;
        c.save(dir.path() + "/depth.gpa_frame");
        Frame frame((dir.path() + "/depth.gpa_frame").toStdWString());
        Experiment project(frame);
        project.setDepthStencil(
            frame, 1000,
            {{"depth_stencil",
              {{"depth_func", 1}, {"stencil_enable", true}, {"front_face", {{"pass_op", 3}}}}},
             {"stencil_ref", UINT32_MAX}});
        project.setDepthStencil(frame, 1000, {{"depth_stencil", {{"front_face", {{"func", 3}}}}}});
        const auto edited = project.depthStencil(frame, 1000);
        QCOMPARE(edited["depth_stencil"]["front_face"]["pass_op"], nlohmann::json(3));
        QCOMPARE(edited["depth_stencil"]["front_face"]["func"], nlohmann::json(3));
        project.save(dir.path() + "/project.json");
        Experiment loaded(frame);
        loaded.load(dir.path() + "/project.json", frame);
        QCOMPARE(loaded.document(), project.document());
        for (bool before : {false, true}) {
            ReplayOptions o;
            o.warp = true;
            o.until = 1000;
            o.before = before;
            loaded.apply(frame, o);
            Replay r(frame, o);
            for (int repeat = 0; repeat < 2; ++repeat) {
                r.run();
                r.inspectNativeState([&](auto *context, const auto &) {
                    Com<ID3D11DepthStencilState> obj;
                    UINT reference;
                    context->OMGetDepthStencilState(&obj, &reference);
                    QVERIFY(obj);
                    D3D11_DEPTH_STENCIL_DESC d;
                    obj->GetDesc(&d);
                    QCOMPARE(d.DepthFunc, D3D11_COMPARISON_NEVER);
                    QCOMPARE(d.FrontFace.StencilPassOp, D3D11_STENCIL_OP_REPLACE);
                    QCOMPARE(d.FrontFace.StencilFunc, D3D11_COMPARISON_EQUAL);
                    QCOMPARE(reference, 255u);
                });
                QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{0, 0, 0, 255}));
                QCOMPARE(storedDepth(r) >> 24, 0x53u);
                QVERIFY((storedDepth(r) & 0xffffff) >= 0x7fffffu);
            }
        }
        ReplayOptions o;
        o.warp = true;
        o.until = 2000;
        loaded.apply(frame, o);
        Replay r(frame, o);
        r.run();
        QCOMPARE(r.output(20).rgba, (std::vector<uint8_t>{255, 0, 0, 255}));
        QCOMPARE(storedDepth(r), 0x53000000u);
        QVERIFY(project.undo());
        QCOMPARE(project.depthStencil(frame, 1000)["depth_stencil"]["front_face"]["func"], nlohmann::json(8));
        QVERIFY(project.redo());
        QCOMPARE(project.depthStencil(frame, 1000), edited);
        auto old = project.document();
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            project.setDepthStencil(frame, 1000, {{"depth_stencil", {{"depth_enable", 1}}}}));
        QCOMPARE(project.document(), old);
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            normalizeDepthStencil({{"depth_test", false}, {"depth_stencil", {{"depth_enable", true}}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 normalizeDepthStencil({{"depth_stencil", {{"front_face", {{"func", 9}}}}}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizeDepthStencil({{"stencil_ref", true}}));
    }
};
QTEST_GUILESS_MAIN(DepthStencilTests)
#include "DepthStencilTests.moc"
