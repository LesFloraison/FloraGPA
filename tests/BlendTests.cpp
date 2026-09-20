#include "BlendCapture.h"
#include "application/BlendEdits.h"
#include "application/RasterizerEdits.h"
#include "replay/BlendState.h"
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <cmath>
using namespace flora;
using Json = nlohmann::json;
namespace {
const std::array<float, 4> S{.5f, .25f, .75f, .5f}, D{.125f, .5f, .25f, .25f}, B{.25f, .5f, .75f, 1.f},
    S1{.25f, .75f, .5f, .25f};
Json values(const Json &row) { return {{"blend_state", {{"targets", {{"0", row}}}}}}; }
std::unique_ptr<Replay> run(const Frame &f, const Json &patch, bool warp, Id event = 100,
                            bool before = false) {
    Experiment project(f);
    if (!patch.empty())
        project.setBlend(f, 100, patch);
    ReplayOptions o;
    o.until = event;
    o.before = before;
    o.warp = warp;
    project.apply(f, o);
    auto r = std::make_unique<Replay>(f, o);
    r->run();
    return r;
}
std::array<float, 4> factor(int n) {
    std::array<float, 4> out{};
    for (size_t i = 0; i < 4; ++i)
        switch (n) {
        case 1:
            out[i] = 0;
            break;
        case 2:
            out[i] = 1;
            break;
        case 3:
            out[i] = S[i];
            break;
        case 4:
            out[i] = 1 - S[i];
            break;
        case 5:
            out[i] = S[3];
            break;
        case 6:
            out[i] = 1 - S[3];
            break;
        case 7:
            out[i] = D[3];
            break;
        case 8:
            out[i] = 1 - D[3];
            break;
        case 9:
            out[i] = D[i];
            break;
        case 10:
            out[i] = 1 - D[i];
            break;
        case 11:
            out[i] = i == 3 ? 1 : std::min(S[3], 1 - D[3]);
            break;
        case 14:
            out[i] = B[i];
            break;
        case 15:
            out[i] = 1 - B[i];
            break;
        case 16:
            out[i] = S1[i];
            break;
        case 17:
            out[i] = 1 - S1[i];
            break;
        case 18:
            out[i] = S1[3];
            break;
        case 19:
            out[i] = 1 - S1[3];
            break;
        default:
            throw std::runtime_error("Unknown test factor");
        }
    return out;
}
float operation(int n, float s, float d) {
    switch (n) {
    case 1:
        return s + d;
    case 2:
        return s - d;
    case 3:
        return d - s;
    case 4:
        return std::min(s, d);
    case 5:
        return std::max(s, d);
    default:
        throw std::runtime_error("Unknown test op");
    }
}
uint32_t logic(int n, uint32_t s, uint32_t d) {
    const uint32_t result[]{0,     UINT32_MAX, s,     ~s,       d,      ~d,     s & d,  ~(s & d),
                            s | d, ~(s | d),   s ^ d, ~(s ^ d), s & ~d, ~s & d, s | ~d, ~s | d};
    return result[n];
}
} // namespace
class BlendTests final : public QObject {
    Q_OBJECT
    void pixel(Replay &r, const std::array<float, 4> &expected, Id id = 1000, UINT sample = 0) {
        auto actual = std::bit_cast<std::array<float, 4>>(testing::blendPixel(r, id, sample));
        for (size_t i = 0; i < 4; ++i)
            QVERIFY2(std::abs(actual[i] - expected[i]) < 1e-6f,
                     qPrintable(QString("channel %1: %2 != %3").arg(i).arg(actual[i]).arg(expected[i])));
    }
  private slots:
    void wireLayouts() {
        auto d = defaultBlend();
        d.AlphaToCoverageEnable = 7;
        d.IndependentBlendEnable = -1;
        d.RenderTarget[0].BlendEnable = 2;
        d.RenderTarget[7].LogicOpEnable = 7;
        auto bytes = testing::statePack(d);
        QCOMPARE(bytes.size(), size_t(328));
        auto decoded = decodeBlend(bytes, true);
        QCOMPARE(decoded.AlphaToCoverageEnable, TRUE);
        QCOMPARE(decoded.IndependentBlendEnable, TRUE);
        QCOMPARE(decoded.RenderTarget[0].BlendEnable, TRUE);
        QCOMPARE(decoded.RenderTarget[7].LogicOpEnable, TRUE);
        bytes.pop_back();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeBlend(bytes, true));
        bytes.push_back(0);
        bytes.push_back(0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeBlend(bytes, true));
        D3D11_BLEND_DESC base{};
        base.AlphaToCoverageEnable = 2;
        base.RenderTarget[3].BlendEnable = -1;
        bytes = testing::statePack(base);
        QCOMPARE(bytes.size(), size_t(264));
        decoded = decodeBlend(bytes, false);
        QCOMPARE(decoded.AlphaToCoverageEnable, TRUE);
        QCOMPARE(decoded.RenderTarget[3].BlendEnable, TRUE);
        QCOMPARE(decoded.RenderTarget[3].LogicOpEnable, FALSE);
        QCOMPARE(decoded.RenderTarget[3].LogicOp, D3D11_LOGIC_OP_NOOP);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, decodeBlend(bytes, true));
    }
    void arithmetic() {
        QTemporaryDir dir;
        for (bool dual : {false, true}) {
            auto path = dir.path() + "/arithmetic.gpa_frame";
            testing::blendCapture(1, dual).save(path);
            Frame f(path.toStdWString());
            for (bool warp : {false, true}) {
                for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19}) {
                    if (dual != (n >= 16))
                        continue;
                    for (bool destination : {false, true}) {
                        auto r = run(f,
                                     values({{"blend_enable", true},
                                             {"src_blend", destination ? 1 : n},
                                             {"dest_blend", destination ? n : 1}}),
                                     warp);
                        auto expected = S;
                        auto k = factor(n);
                        for (size_t i = 0; i < 3; ++i)
                            expected[i] = (destination ? D[i] : S[i]) * k[i];
                        pixel(*r, expected);
                    }
                    if (n != 3 && n != 4 && n != 9 && n != 10 && n != 16 && n != 17) {
                        auto r = run(f, values({{"blend_enable", true}, {"src_blend_alpha", n}}), warp);
                        auto expected = S;
                        expected[3] *= factor(n)[3];
                        pixel(*r, expected);
                    }
                }
                if (dual)
                    continue;
                for (int op = 1; op <= 5; ++op) {
                    auto r = run(f,
                                 values({{"blend_enable", true},
                                         {"src_blend", 2},
                                         {"dest_blend", 2},
                                         {"blend_op", op},
                                         {"src_blend_alpha", 2},
                                         {"dest_blend_alpha", 2},
                                         {"blend_op_alpha", op}}),
                                 warp);
                    auto expected = S;
                    for (size_t i = 0; i < 4; ++i)
                        expected[i] = operation(op, S[i], D[i]);
                    pixel(*r, expected);
                }
                for (int mask = 0; mask < 16; ++mask) {
                    auto r = run(f, values({{"write_mask", mask}}), warp);
                    auto expected = D;
                    for (size_t i = 0; i < 4; ++i)
                        if (mask & (1 << i))
                            expected[i] = S[i];
                    pixel(*r, expected);
                }
                auto v = values({{"blend_enable", true}, {"src_blend", 14}, {"src_blend_alpha", 14}});
                v["blend_factor"] = {.5, .25, 1, .75};
                auto r = run(f, v, warp);
                pixel(*r, {.25f, .0625f, .75f, .375f});
            }
        }
    }
    void independentTargetsAndLogic() {
        QTemporaryDir dir;
        auto path = dir.path() + "/mrt.gpa_frame";
        testing::blendCapture(8).save(path);
        Frame f(path.toStdWString());
        Json targets = Json::object();
        for (int i = 0; i < 8; ++i)
            targets[std::to_string(i)] = {{"blend_enable", true},
                                          {"src_blend", 2},
                                          {"dest_blend", 2},
                                          {"blend_op", 1 + i % 5},
                                          {"write_mask", i + 1}};
        for (bool warp : {false, true}) {
            auto r = run(f, {{"blend_state", {{"independent_blend", true}, {"targets", targets}}}}, warp);
            for (int i = 0; i < 8; ++i) {
                auto expected = D;
                for (int k = 0; k < 4; ++k)
                    if ((i + 1) & (1 << k))
                        expected[k] = k == 3 ? S[k] : operation(1 + i % 5, S[k], D[k]);
                pixel(*r, expected, 1000 + 4 * i);
            }
        }
        const std::array<uint32_t, 4> source{0xabcdef12u, 0x13579bdfu, 0xff00ff00u, 0x80000001u},
            destination{0x0f0f0f0fu, 0x2468ace0u, 0x55aa55aau, 0x7fffffffu};
        for (bool captured : {false, true}) {
            auto file = dir.path() + "/logic.gpa_frame";
            testing::blendCapture(1, false, true, 1, captured ? 10 : -1).save(file);
            Frame frame(file.toStdWString());
            for (bool warp : {false, true})
                for (int op = 0; op < 16; ++op) {
                    if (captured && op != 10)
                        continue;
                    auto r =
                        run(frame,
                            captured ? Json::object() : values({{"logic_op_enable", true}, {"logic_op", op}}),
                            warp);
                    auto actual = testing::blendPixel(*r);
                    for (size_t i = 0; i < 4; ++i)
                        QCOMPARE(actual[i], logic(op, source[i], destination[i]));
                }
        }
        for (bool warp : {false, true})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, run(f, values({{"logic_op_enable", true}}), warp));
    }
    void samplesAndAlphaCoverage() {
        QTemporaryDir dir;
        auto path = dir.path() + "/msaa.gpa_frame";
        testing::blendCapture(1, false, false, 4).save(path);
        Frame f(path.toStdWString());
        for (bool warp : {false, true}) {
            for (uint32_t mask : {0u, 1u, 5u, 15u, UINT32_MAX}) {
                auto r = run(f, {{"sample_mask", mask}}, warp);
                for (UINT i = 0; i < 4; ++i)
                    pixel(*r, mask & (1u << i) ? S : D, 1000, i);
            }
            auto edited = run(f, {{"blend_state", {{"alpha_to_coverage", true}}}}, warp);
            std::array<std::array<uint32_t, 4>, 4> actual;
            for (UINT i = 0; i < 4; ++i)
                actual[i] = testing::blendPixel(*edited, 1000, i);
            auto direct = run(f, Json::object(), warp, 100, true);
            direct->inspectNativeState([&](auto *context, const auto &) {
                Com<ID3D11Device> device;
                context->GetDevice(&device);
                D3D11_BLEND_DESC desc{};
                desc.AlphaToCoverageEnable = TRUE;
                for (auto &rt : desc.RenderTarget) {
                    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                    rt.RenderTargetWriteMask = 15;
                }
                Com<ID3D11BlendState> state;
                check(device->CreateBlendState(&desc, &state), "Independent AToC state");
                context->OMSetBlendState(state.Get(), B.data(), UINT32_MAX);
                context->Draw(3, 0);
            });
            bool different = false;
            for (UINT i = 0; i < 4; ++i) {
                QCOMPARE(actual[i], testing::blendPixel(*direct, 1000, i));
                different |= actual[i] != std::bit_cast<std::array<uint32_t, 4>>(S);
            }
            QVERIFY(different);
        }
    }
    void historyAndValidation() {
        QTemporaryDir dir;
        auto path = dir.path() + "/history.gpa_frame";
        testing::blendCapture(1, false, false, 1, -1, true).save(path);
        Frame f(path.toStdWString());
        Experiment project(f);
        project.setBlend(f, 100, values({{"blend_enable", true}, {"src_blend", 5}}));
        project.setBlend(f, 100, {{"blend_disabled", true}});
        project.setBlend(f, 100, values({{"dest_blend", 6}}));
        project.setBlend(f, 100,
                         {{"blend_disabled", false},
                          {"sample_mask", 0u},
                          {"depth_test", false},
                          {"rasterizer", {{"cull_mode", 1}}}});
        auto b = project.blend(f, 100);
        QCOMPARE(b["blend_state"]["targets"]["0"]["blend_enable"], Json(false));
        QCOMPARE(b["blend_state"]["targets"]["0"]["src_blend"], Json(5));
        QCOMPARE(b["blend_state"]["targets"]["0"]["dest_blend"], Json(6));
        project.save(dir.path() + "/history.json");
        Experiment loaded(f);
        loaded.load(dir.path() + "/history.json", f);
        QCOMPARE(loaded.document(), project.document());
        for (auto boundary : {std::pair<Id, bool>{100, false}, {100, true}, {200, false}}) {
            ReplayOptions o;
            o.warp = true;
            o.until = boundary.first;
            o.before = boundary.second;
            loaded.apply(f, o);
            Replay replay(f, o);
            for (int i = 0; i < 2; ++i) {
                replay.run();
                replay.inspectNativeState([&](auto *context, const auto &) {
                    UINT mask;
                    context->OMGetBlendState(nullptr, nullptr, &mask);
                    QCOMPARE(mask, boundary.first == 100 ? 0u : UINT32_MAX);
                });
                pixel(replay, boundary.first == 100 ? D : S);
            }
        }
        QVERIFY(project.undo());
        QCOMPARE(project.blend(f, 100)["sample_mask"], Json(UINT32_MAX));
        QVERIFY(project.redo());
        QCOMPARE(project.blend(f, 100), b);
        QVERIFY(project.undo());
        auto before = project.document();
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            project.setBlend(f, 100, values({{"blend_enable", true}, {"logic_op_enable", true}})));
        QCOMPARE(project.document(), before);
        QVERIFY(project.redo());
        for (const auto &bad :
             std::vector<Json>{values({{"src_blend", 12}}),
                               values({{"src_blend_alpha", 3}}),
                               values({{"logic_op", 16}}),
                               values({{"write_mask", true}}),
                               {{"sample_mask", -1}},
                               {{"sample_mask", uint64_t(UINT32_MAX) + 1}},
                               {{"blend_factor", {0, 1, 2, 0}}},
                               {{"blend_disabled", true}, {"blend_state", {{"alpha_to_coverage", true}}}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, normalizePipeline(bad));
    }
};
QTEST_GUILESS_MAIN(BlendTests)
#include "BlendTests.moc"
