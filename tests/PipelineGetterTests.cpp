#include "SyntheticCapture.h"
#include "application/ApiCommands.h"
#include "application/FrameValidation.h"
#include "core/PipelineGetters.h"
#include "core/ReplayCapabilities.h"
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
namespace {
using Raw = std::vector<uint8_t>;
QString fixture(int mode) {
    auto root =
        qEnvironmentVariable(mode < 8 ? "FLORA_PIPELINE_GETTERS" : "FLORA_PIPELINE_GETTER_BOUNDARIES");
    return root.isEmpty() ? QString{} : root + QString("/%1/").arg(mode);
}
Raw bytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing getter oracle");
    auto b = file.readAll();
    return {reinterpret_cast<const uint8_t *>(b.data()),
            reinterpret_cast<const uint8_t *>(b.data()) + b.size()};
}
void copy(const Frame &f, const QString &path, Id edited, const Raw &replacement) {
    Capture capture;
    for (const auto &[id, e] : f.entries()) {
        auto raw = f.payload(id);
        capture.add(id, e.category, e.type, id == edited ? replacement : Raw(raw.begin(), raw.end()));
    }
    capture.save(path);
}
// Observe native identities and scalar state independently of the captured wire decoder.
// Every Get* reference is released; the context keeps the bound objects alive.
Raw bindings(ID3D11DeviceContext *c) {
    Raw out;
    auto object = [&](auto *p) {
        append(out, uintptr_t(p));
        if (p)
            p->Release();
    };
    auto objects = [&](auto &a) {
        for (auto *p : a)
            object(p);
    };
#define STAGE(P, T)                                                                                          \
    {                                                                                                        \
        T *shader{};                                                                                         \
        ID3D11ClassInstance *classes[256]{};                                                                 \
        UINT count = 256;                                                                                    \
        c->P##GetShader(&shader, classes, &count);                                                           \
        object(shader);                                                                                      \
        append(out, count);                                                                                  \
        objects(classes);                                                                                    \
        ID3D11Buffer *cb[14]{};                                                                              \
        c->P##GetConstantBuffers(0, 14, cb);                                                                 \
        objects(cb);                                                                                         \
        ID3D11ShaderResourceView *srv[128]{};                                                                \
        c->P##GetShaderResources(0, 128, srv);                                                               \
        objects(srv);                                                                                        \
        ID3D11SamplerState *samplers[16]{};                                                                  \
        c->P##GetSamplers(0, 16, samplers);                                                                  \
        objects(samplers);                                                                                   \
    }
    STAGE(VS, ID3D11VertexShader)
    STAGE(HS, ID3D11HullShader)
    STAGE(DS, ID3D11DomainShader)
    STAGE(GS, ID3D11GeometryShader)
    STAGE(PS, ID3D11PixelShader)
    STAGE(CS, ID3D11ComputeShader)
#undef STAGE
    ID3D11Buffer *vb[32]{}, *ib{}, *so[4]{};
    UINT strides[32]{}, offsets[32]{}, offset{};
    DXGI_FORMAT format{};
    c->IAGetVertexBuffers(0, 32, vb, strides, offsets);
    objects(vb);
    append(out, strides);
    append(out, offsets);
    c->IAGetIndexBuffer(&ib, &format, &offset);
    object(ib);
    append(out, format);
    append(out, offset);
    ID3D11InputLayout *layout{};
    c->IAGetInputLayout(&layout);
    object(layout);
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    c->IAGetPrimitiveTopology(&topology);
    append(out, topology);
    c->SOGetTargets(4, so);
    objects(so);
    ID3D11RasterizerState *rs{};
    c->RSGetState(&rs);
    object(rs);
    D3D11_RECT rects[16]{};
    UINT count = 16;
    c->RSGetScissorRects(&count, rects);
    append(out, count);
    append(out, rects);
    D3D11_VIEWPORT viewports[16]{};
    count = 16;
    c->RSGetViewports(&count, viewports);
    append(out, count);
    append(out, viewports);
    ID3D11RenderTargetView *rtvs[8]{};
    ID3D11DepthStencilView *dsv{};
    c->OMGetRenderTargets(8, rtvs, &dsv);
    objects(rtvs);
    object(dsv);
    // Query OM UAVs separately so the read itself has no RTV/UAV slot overlap.
    ID3D11UnorderedAccessView *uavs[8]{};
    c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
    objects(uavs);
    std::fill(std::begin(uavs), std::end(uavs), nullptr);
    c->CSGetUnorderedAccessViews(0, 8, uavs);
    objects(uavs);
    ID3D11Predicate *predicate{};
    BOOL value{};
    c->GetPredication(&predicate, &value);
    const bool bound = predicate != nullptr;
    object(predicate);
    append(out, bound ? value : FALSE);
    append(out, c->GetType());
    append(out, c->GetContextFlags());
    return out;
}
} // namespace
class PipelineGetterTests final : public QObject {
    Q_OBJECT
  private slots:
    void originals_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("warp");
        for (int mode : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12})
            for (bool warp : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(mode).arg(warp ? "warp" : "hardware")))
                    << mode << warp;
    }
    void originals() {
        QFETCH(int, mode);
        QFETCH(bool, warp);
        const auto root = fixture(mode);
        if (root.isEmpty())
            QSKIP("Set original getter capture roots");
        Frame frame((root + "capture.gpa_frame").toStdWString());
        const auto validation = validateFrame(frame.path());
        QVERIFY2(validation["errors"] == 0, validation.dump(2).c_str());
        std::set<Id> getters;
        for (const auto &[id, e] : frame.entries())
            if (e.category == 7 && isPipelineGetter(e.type)) {
                getters.insert(id);
                const auto command = inspectCommand(frame, id);
                QCOMPARE(command["status"], nlohmann::json("decoded"));
                QCOMPARE(std::string(replayCapability(e.type).handling), std::string("metadata"));
            }
        QVERIFY(!getters.empty());
        const auto expected = bytes(root + "native/expected.rgba");
        ReplayOptions options;
        options.warp = warp;
        Replay replay(frame, options);
        for (int repeat = 0; repeat < 2; ++repeat) {
            Raw before;
            size_t checked = 0;
            replay.run({}, {}, [&](Id id, bool after, auto *context, const auto &) {
                if (!getters.contains(id))
                    return;
                if (!after)
                    before = bindings(context);
                else {
                    QCOMPARE(bindings(context), before);
                    ++checked;
                }
            });
            QCOMPARE(checked, getters.size());
            QCOMPARE(replay.counts["pipeline_getter_observations"], getters.size());
            QCOMPARE(replay.output().rgba, expected);
            if (mode == 12) {
                for (const auto &[id, e] : frame.entries())
                    if (e.category == 7 && e.type == 0x35) {
                        const auto state = frame.state(frame.event(id).state);
                        Reader view(frame.payload(state.csUav[0]));
                        view.skip(16);
                        QCOMPARE(replay.readBuffer(view.read<Id>()), bytes(root + "native/buffer1.bin"));
                    }
            }
        }
        options.disabled = getters;
        Replay disabled(frame, options);
        disabled.run();
        QCOMPARE(disabled.output().rgba, expected);
        QCOMPARE(disabled.counts["pipeline_getter_observations"], uint64_t(0));
    }
    void wireBoundsAndDiagnostics() {
        if (fixture(0).isEmpty())
            QSKIP("Set original getter captures");
        QTemporaryDir temp;
        std::set<uint16_t> types;
        size_t truncated = 0, flags = 0, counts = 0, negatives = 0;
        for (int mode = 0; mode < 13; ++mode) {
            const auto root = fixture(mode);
            if (root.isEmpty())
                QSKIP("Set original getter boundary captures");
            Frame frame((root + "capture.gpa_frame").toStdWString());
            for (const auto &[id, e] : frame.entries())
                if (e.category == 7 && isPipelineGetter(e.type)) {
                    const auto raw = frame.payload(id);
                    const auto record = readPipelineGetter(e.type, raw);
                    for (size_t n = 0; n < raw.size(); ++n) {
                        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                                 readPipelineGetter(e.type, raw.first(n)));
                        ++truncated;
                    }
                    Raw extra(raw.begin(), raw.end());
                    extra.push_back(0);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineGetter(e.type, extra));
                    for (const auto &f : record.fields) {
                        if (f.format == "B") {
                            Raw changed(raw.begin(), raw.end());
                            changed[f.offset] = 2;
                            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineGetter(e.type, changed));
                            ++flags;
                        }
                        if (f.format == "I" && (f.name.find("count") != std::string::npos ||
                                                f.name == "start_slot" || f.name == "uav_start")) {
                            Raw changed(raw.begin(), raw.end());
                            put(changed, f.offset, UINT32_MAX);
                            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineGetter(e.type, changed));
                            ++counts;
                        }
                    }
                    if (!types.insert(e.type).second)
                        continue;
                    for (int bad = 0; bad < 5; ++bad) {
                        Raw changed(raw.begin(), raw.end());
                        if (bad == 0)
                            put(changed, 0, Id(1));
                        if (bad == 1)
                            put(changed, 8, Id(0));
                        if (bad == 2)
                            put(changed, 8, Id(UINT64_MAX));
                        if (bad == 3)
                            changed.pop_back();
                        if (bad == 4)
                            put(changed, 8, id); // An event is not a context resource.
                        const auto path = temp.filePath(QString("bad-%1-%2.gpa_frame").arg(e.type).arg(bad));
                        copy(frame, path, id, changed);
                        Frame malformed(path.toStdWString());
                        QCOMPARE(inspectCommand(malformed, id)["status"], nlohmann::json("invalid"));
                        const auto report = validateFrame(malformed.path());
                        QVERIFY(report["errors"] != 0);
                        QVERIFY(std::any_of(report["findings"].begin(), report["findings"].end(),
                                            [&](const auto &finding) {
                                                return finding["severity"] == "error" &&
                                                       finding.value("event_id", Id(0)) == id;
                                            }));
                        ReplayOptions options;
                        options.warp = true;
                        Replay replay(malformed, options);
                        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay.run());
                        ++negatives;
                    }
                }
        }
        QCOMPARE(types.size(), size_t(34));
        qInfo() << "truncated prefixes" << truncated << "invalid flags" << flags << "invalid counts" << counts
                << "whole-frame negatives" << negatives;
    }
    void observationDoesNotRestoreBindings() {
        const auto root = fixture(0);
        if (root.isEmpty())
            QSKIP("Set original getter captures");
        Frame original((root + "capture.gpa_frame").toStdWString());
        Capture capture;
        unsigned edited = 0;
        for (const auto &[id, e] : original.entries()) {
            const auto raw = original.payload(id);
            Raw changed(raw.begin(), raw.end());
            if (e.category == 7 && isPipelineGetter(e.type)) {
                for (const auto &field : readPipelineGetter(e.type, raw).fields)
                    if (field.reference) {
                        put(changed, field.offset, Id(UINT64_MAX));
                        ++edited;
                    }
            }
            capture.add(id, e.category, e.type, changed);
        }
        QVERIFY(edited != 0);
        QTemporaryDir temp;
        const auto path = temp.filePath("observations.gpa_frame");
        capture.save(path);
        Frame frame(path.toStdWString());
        QVERIFY(validateFrame(frame.path())["errors"] == 0);
        ReplayOptions options;
        options.warp = true;
        Replay replay(frame, options);
        replay.run();
        QCOMPARE(replay.output().rgba, bytes(root + "native/expected.rgba"));
    }
    void absentArrayCount() {
        Raw raw(16);
        append(raw, Id(0));      // Returned shader.
        append(raw, uint8_t(0)); // No saved count.
        append(raw, uint8_t(1)); // Array claims to be present.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineGetter(0x352a, raw));
        raw.resize(16);
        append(raw, uint8_t(0));
        append(raw, uint8_t(1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readPipelineGetter(0x353e, raw));
    }
    void missingSoResource() {
        const auto path = qEnvironmentVariable("FLORA_PIPELINE_GETTER_MISSING_SO");
        if (path.isEmpty())
            QSKIP("Set original incomplete SO capture");
        Frame frame(path.toStdWString());
        const auto report = validateFrame(frame.path());
        QCOMPARE(report["errors"], nlohmann::json(0));
        bool found = false;
        for (const auto &f : report["findings"])
            if (f["kind"] == "stream_output_unused_binding") {
                QCOMPARE(f["event_id"], nlohmann::json(43));
                QCOMPARE(f["resource_id"], nlohmann::json(44));
                found = true;
            }
        QVERIFY(found);
        for (bool warp : {false, true}) {
            ReplayOptions options;
            options.warp = warp;
            Replay replay(frame, options);
            replay.run();
            QCOMPARE(replay.unusedStreamOutputLifetimes().size(), size_t(1));
            QCOMPARE(replay.unusedStreamOutputLifetimes()[0].closingEvent, Id(47));
            QCOMPARE(replay.output().rgba, bytes(QFileInfo(path).path() + "/native/expected.rgba"));
        }
    }
};
QTEST_GUILESS_MAIN(PipelineGetterTests)
#include "PipelineGetterTests.moc"
