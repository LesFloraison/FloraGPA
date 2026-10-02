#include "PipelineSetterCapture.h"
#include "application/ApiCommands.h"
#include "application/Experiment.h"
#include "application/FrameValidation.h"
#include "application/SetterEdits.h"
#include "core/InspectionRecords.h"
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
    static void verifyBinding(const PipelineBinding &binding, ID3D11DeviceContext *ctx,
                              const std::map<Id, Com<IUnknown>> &objects) {
        const auto &s = binding.values;
        auto identity = [&](IUnknown *object, Id id) {
            Com<IUnknown> identity;
            if (object)
                check(object->QueryInterface(IID_PPV_ARGS(&identity)), "Get binding identity");
            Com<IUnknown> expected;
            if (id)
                check(objects.at(id).As(&expected), "Get expected identity");
            QCOMPARE(identity.Get(), expected.Get());
        };
        if (auto stage = shaderSetterStage(binding.type)) {
            std::array<ID3D11ClassInstance *, 256> classes{};
            UINT count = UINT(classes.size());
#define CHECK_SHADER(Stage, Interface)                                                                       \
    {                                                                                                        \
        Com<Interface> shader;                                                                               \
        ctx->Stage##GetShader(&shader, classes.data(), &count);                                              \
        identity(shader.Get(), s.stages[*stage].shader);                                                     \
    }
            switch (*stage) {
            case 0:
                CHECK_SHADER(VS, ID3D11VertexShader);
                break;
            case 1:
                CHECK_SHADER(HS, ID3D11HullShader);
                break;
            case 2:
                CHECK_SHADER(DS, ID3D11DomainShader);
                break;
            case 3:
                CHECK_SHADER(GS, ID3D11GeometryShader);
                break;
            case 4:
                CHECK_SHADER(PS, ID3D11PixelShader);
                break;
            case 5:
                CHECK_SHADER(CS, ID3D11ComputeShader);
                break;
            }
#undef CHECK_SHADER
            const auto expected = s.stages[*stage].classCount;
            for (UINT i = 0; i < count; ++i) {
                if (i < expected)
                    identity(classes[i], s.stages[*stage].classes[i]);
                if (classes[i])
                    classes[i]->Release();
            }
            QCOMPARE(count, expected);
        }
        switch (binding.type - 0x34de) {
        case 24: {
            D3D11_PRIMITIVE_TOPOLOGY topology;
            ctx->IAGetPrimitiveTopology(&topology);
            QCOMPARE(UINT(topology), s.topology);
            break;
        }
        case 35: {
            Com<ID3D11BlendState> state;
            std::array<float, 4> factor;
            UINT mask;
            ctx->OMGetBlendState(&state, factor.data(), &mask);
            identity(state.Get(), s.blend);
            QCOMPARE(factor, s.blendFactor);
            QCOMPARE(mask, s.sampleMask);
            break;
        }
        case 36: {
            Com<ID3D11DepthStencilState> state;
            UINT ref;
            ctx->OMGetDepthStencilState(&state, &ref);
            identity(state.Get(), s.depthState);
            QCOMPARE(ref, s.stencilRef);
            break;
        }
        case 43: {
            Com<ID3D11RasterizerState> state;
            ctx->RSGetState(&state);
            identity(state.Get(), s.rasterizer);
            break;
        }
        case 44: {
            UINT count = 16;
            D3D11_VIEWPORT rows[16]{};
            ctx->RSGetViewports(&count, rows);
            QCOMPARE(size_t(count), s.viewportValues->size());
            for (UINT i = 0; i < count; ++i) {
                std::array<float, 6> row;
                std::memcpy(row.data(), &rows[i], sizeof rows[i]);
                QCOMPARE(row, s.viewportValues->at(i));
            }
            break;
        }
        case 45: {
            UINT count = 16;
            D3D11_RECT rows[16]{};
            ctx->RSGetScissorRects(&count, rows);
            QCOMPARE(size_t(count), s.scissorValues->size());
            for (UINT i = 0; i < count; ++i) {
                std::array<int32_t, 4> row;
                std::memcpy(row.data(), &rows[i], sizeof rows[i]);
                QCOMPARE(row, s.scissorValues->at(i));
            }
            break;
        }
        }
    }
  private slots:
    void outputGetterRecords() {
        QTemporaryDir dir;
        for (uint16_t type : {0x3539, 0x353a})
            for (unsigned flags = 0; flags < 4; ++flags) {
                auto raw = statePack(Id(0), Id(1), Id(0x123456789));
                append(raw, uint8_t(flags & 1));
                if (flags & 1) {
                    if (type == 0x3539) {
                        for (float x : {.25f, .5f, .75f, 1.f})
                            append(raw, x);
                    } else
                        append(raw, 0xa5u);
                }
                if (type == 0x3539) {
                    append(raw, uint8_t((flags >> 1) & 1));
                    if (flags & 2)
                        append(raw, 0xffffffffu);
                }
                QVERIFY(acceptInspectionRecord(type, raw));
                for (size_t n = 0; n < raw.size(); ++n)
                    QVERIFY_THROWS_EXCEPTION(std::exception,
                                             acceptInspectionRecord(type, Bytes(raw).first(n)));
                auto extra = raw;
                extra.push_back(0);
                QVERIFY_THROWS_EXCEPTION(std::exception, acceptInspectionRecord(type, extra));
                auto invalid = raw;
                invalid[24] = 2;
                QVERIFY_THROWS_EXCEPTION(std::exception, acceptInspectionRecord(type, invalid));
                auto c = pipelineSetterCapture();
                c.add(96, 7, type, raw);
                auto path = dir.filePath("getter.gpa_frame");
                c.save(path);
                Frame frame(path.toStdWString());
                QCOMPARE(inspectCommand(frame, 96)["status"], Json("decoded"));
                ReplayOptions options;
                options.warp = true;
                options.until = 96;
                Replay replay(frame, options);
                replay.run();
                replay.inspectNativeState([&](auto *ctx, const auto &objects) {
                    verifyBinding(readPipelineSetter(0x3501, frame.payload(91)), ctx, objects);
                    verifyBinding(readPipelineSetter(0x3502, frame.payload(92)), ctx, objects);
                });
            }
    }
    void capturedBoundaries() {
        QTemporaryDir dir;
        auto c = pipelineSetterCapture();
        c.add(81, 7, 0x34e9, statePack(Id(0), Id(1), Id(10), 0u, uint8_t(0)));
        c.add(82, 7, 0x34e7, statePack(Id(0), Id(1), Id(30), 0u, uint8_t(0)));
        for (auto [id, type] : std::map<Id, uint16_t>{{83, 0x351a}, {84, 0x351e}, {85, 0x34f5}, {86, 0x3523}})
            c.add(id, 7, type, statePack(Id(0), Id(1), Id(0), 0u, uint8_t(0)));
        c.add(96, 7, 0x350a, statePack(Id(0), Id(1), 0u, uint8_t(0)));
        c.add(97, 7, 0x350b, statePack(Id(0), Id(1), 0u, uint8_t(0)));
        auto path = dir.filePath("boundaries.gpa_frame");
        c.save(path);
        Frame frame(path.toStdWString());
        for (bool warp : {false, true}) {
            ReplayOptions options;
            options.warp = warp;
            options.until = 97;
            Replay replay(frame, options);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                size_t checked = 0;
                replay.run({}, {}, [&](Id id, bool after, auto *ctx, const auto &objects) {
                    const auto &e = frame.entry(id);
                    if (after && isPipelineSetter(e.type)) {
                        verifyBinding(readPipelineSetter(e.type, frame.payload(id)), ctx, objects);
                        ++checked;
                    }
                });
                QCOMPARE(checked, size_t(14));
                QCOMPARE(replay.counts.at("pipeline_setter_records"), uint64_t(14));
            }
        }
    }
    void strictCapturedRecords() {
        auto c = pipelineSetterCapture();
        for (auto [id, type] : std::map<Id, uint16_t>{
                 {81, 0x34e9}, {82, 0x34e7}, {83, 0x351a}, {84, 0x351e}, {85, 0x34f5}, {86, 0x3523}})
            c.add(id, 7, type, statePack(Id(0), Id(1), Id(0), 0u, uint8_t(0)));
        QTemporaryDir dir;
        auto path = dir.filePath("invalid.gpa_frame");
        for (const auto &e : c.entries) {
            if (e.category != 7 || !isPipelineSetter(e.type))
                continue;
            std::vector<uint8_t> raw(c.bytes.begin() + e.offset, c.bytes.begin() + e.offset + e.size);
            for (size_t n = 0; n < raw.size(); ++n)
                QVERIFY_EXCEPTION_THROWN(readPipelineSetter(e.type, Bytes(raw).first(n)), std::exception);
            auto extra = raw;
            extra.push_back(0);
            QVERIFY_EXCEPTION_THROWN(readPipelineSetter(e.type, extra), std::exception);
            put(raw, 0, Id(123));
            QVERIFY_EXCEPTION_THROWN(readPipelineSetter(e.type, raw), std::exception);
        }
        for (auto raw : {statePack(Id(0), Id(1), 6u), statePack(Id(0), Id(987), 4u),
                         statePack(Id(0), Id(1), 4u, uint8_t(0))}) {
            auto broken = pipelineSetterCapture();
            broken.add(89, 7, 0x34f6, raw);
            broken.save(path);
            Frame frame(path.toStdWString());
            QCOMPARE(validateFrame(path.toStdWString())["status"], Json("blocked"));
            ReplayOptions options;
            options.warp = true;
            options.until = 89;
            Replay replay(frame, options);
            QVERIFY_EXCEPTION_THROWN(replay.run(), std::exception);
        }
    }
    void missingShaderBoundaries() {
        QTemporaryDir dir;
        auto path = dir.filePath("missing.gpa_frame");
        for (int recovery = 0; recovery < 3; ++recovery) {
            auto c = pipelineSetterCapture();
            c.add(84, 7, 0x34e9, statePack(Id(0), Id(1), Id(10), 0u, uint8_t(0)));
            c.add(85, 7, 0x34e9, statePack(Id(0), Id(1), Id(987), 0u, uint8_t(0)));
            if (recovery == 1)
                c.add(86, 7, 0x34e9, statePack(Id(0), Id(1), Id(10), 0u, uint8_t(0)));
            if (recovery == 2)
                c.add(86, 7, 0x242, statePack(Id(0), Id(1)));
            c.save(path);
            Frame frame(path.toStdWString());
            const auto report = validateFrame(path.toStdWString());
            QVERIFY(report["status"] != "blocked");
            bool found = false;
            for (const auto &finding : report["findings"])
                if (finding["kind"] == "shader_binding_not_saved") {
                    QCOMPARE(finding["event_id"], Json(85));
                    QCOMPARE(finding["resource_id"], Json(987));
                    found = true;
                }
            QVERIFY(found);
            const auto edit = readPipelineSetter(0x34e9, statePack(Id(0), Id(1), Id(0), 0u, uint8_t(0)));
            const auto effective =
                pipelineBindingsAt(frame, 100, frame.state(frame.event(100).state), {{84, edit}});
            QCOMPARE(effective.stages[0].shader, Id(10));
            ReplayOptions options;
            options.warp = true;
            options.pipelineSetters[84] = edit;
            options.until = 85;
            Replay gap(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, gap.run());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, gap.inspectNativeState([](auto *, const auto &) {}));
            options.until = recovery ? 86 : 100;
            options.before = !recovery;
            Replay recovered(frame, options);
            recovered.run();
            recovered.inspectNativeState([&](auto *ctx, const auto &objects) {
                verifyBinding(readPipelineSetter(0x34e9, statePack(Id(0), Id(1), Id(recovery == 2 ? 0 : 10),
                                                                   0u, uint8_t(0))),
                              ctx, objects);
            });
        }
        for (Id bad : {Id(20), Id(987)}) {
            auto c = pipelineSetterCapture();
            c.add(85, 7, 0x34e9, statePack(Id(0), Id(1), bad, 1u, uint8_t(1), Id(10)));
            c.save(path);
            Frame frame(path.toStdWString());
            QCOMPARE(validateFrame(path.toStdWString())["status"], Json("blocked"));
            ReplayOptions options;
            options.warp = true;
            options.until = 85;
            Replay rejected(frame, options);
            QVERIFY_THROWS_EXCEPTION(std::exception, rejected.run());
        }
    }
    void originalCapturedBoundaries() {
        const auto root = qEnvironmentVariable("FLORA_PIPELINE_CAPTURES");
        if (root.isEmpty())
            QSKIP("Set FLORA_PIPELINE_CAPTURES to the original pipeline producer outputs");
        std::set<uint16_t> covered;
        for (int mode : {0, 1, 4})
            for (bool warp : {false, true}) {
                Frame frame((root + QString("/%1/capture.gpa_frame").arg(mode)).toStdWString());
                ReplayOptions options;
                options.warp = warp;
                Replay replay(frame, options);
                size_t checked = 0;
                replay.run({}, {}, [&](Id id, bool after, auto *ctx, const auto &objects) {
                    const auto &e = frame.entry(id);
                    if (after && isPipelineSetter(e.type)) {
                        verifyBinding(readPipelineSetter(e.type, frame.payload(id)), ctx, objects);
                        ++checked;
                        covered.insert(e.type);
                    }
                });
                QVERIFY(checked >= 12);
                QCOMPARE(replay.counts.at("pipeline_setter_records"), uint64_t(checked));
            }
        QCOMPARE(covered.size(), size_t(12));
    }
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
