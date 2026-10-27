#include "BlendCapture.h"
#include "DepthStencilCapture.h"
#include "StreamCapture.h"
#include "application/NativePixelGeometryReplay.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
ReplayOptions options(uint32_t vendor, Id event) {
    ReplayOptions o;
    o.debug = true;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.until = event;
    return o;
}
void devices() {
    QTest::addColumn<uint32_t>("vendor");
    for (uint32_t vendor : {0u, 4318u, 32902u})
        QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
}
ID3D11DeviceContext *context(Replay &r) {
    ID3D11DeviceContext *result = nullptr;
    r.inspectNativeState([&](auto *native, const auto &) { result = native; });
    return result;
}
void drawCount(Capture &c, Id event, uint32_t count) {
    for (const auto &e : c.entries)
        if (e.id == event)
            put(c.bytes, size_t(e.offset) + 24, count);
}
void evidence(const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_GEOMETRY_REPLAY_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create geometry replay evidence directory");
    QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                                   QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto data = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(data.data(), qint64(data.size())) != qint64(data.size()))
        throw std::runtime_error("Cannot save geometry replay evidence");
}
D3D11_DEPTH_STENCIL_DESC depthState() {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.StencilReadMask = d.StencilWriteMask = 255;
    d.FrontFace = d.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                D3D11_COMPARISON_ALWAYS};
    return d;
}
} // namespace
class NativePixelGeometryReplayTests final : public QObject {
    Q_OBJECT
  private slots:
    void checkpoints_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"ordinary", "depth", "depth_reject", "stencil", "stencil_reject",
                                     "discard", "early_discard", "blend", "color_mask", "sample_mask",
                                     "scissor", "inputs", "partial_position"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void checkpoints() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = depthStencilCapture();
        drawCount(c, 1000, 6);
        auto o = options(vendor, 1000);
        o.shaders[30] = compileClassProgram("float4 main(uint v:SV_VertexID):SV_Position{uint i=v%3;return "
                                            "float4(i==2?3:-1,i==1?3:-1,.25-v/3*.125,1);}",
                                            "vs_5_0");
        auto depth = depthState();
        if (mode == "depth" || mode == "depth_reject") {
            depth.DepthEnable = TRUE;
            depth.DepthFunc = mode == "depth" ? D3D11_COMPARISON_LESS : D3D11_COMPARISON_NEVER;
        }
        if (mode == "stencil" || mode == "stencil_reject") {
            depth.StencilEnable = TRUE;
            depth.FrontFace = depth.BackFace = {
                D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
                mode == "stencil" ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_NEVER};
        }
        o.depthStencilEdits[1000].descriptor = depth;
        if (mode == "sample_mask")
            o.blendEdits[1000].sampleMask = 0;
        if (mode == "blend" || mode == "color_mask") {
            D3D11_BLEND_DESC1 d{};
            for (auto &t : d.RenderTarget) {
                t.BlendEnable = mode == "blend";
                t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE;
                t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ONE;
                t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                t.RenderTargetWriteMask = mode == "color_mask" ? 0 : 15;
            }
            o.blendEdits[1000].descriptor = d;
            o.shaders[32] =
                compileClassProgram("float4 main():SV_Target{return float4(.25,.125,.375,.25);}", "ps_5_0");
        }
        if (mode == "scissor") {
            D3D11_RASTERIZER_DESC2 d{};
            d.FillMode = D3D11_FILL_SOLID;
            d.CullMode = D3D11_CULL_NONE;
            d.DepthClipEnable = d.ScissorEnable = TRUE;
            o.rasterizerEdits[1000].descriptor = d;
            o.rasterizerEdits[1000].scissors = std::vector<D3D11_RECT>{{1, 1, 2, 2}};
        }
        if (mode == "discard" || mode == "early_discard")
            o.shaders[32] = compileClassProgram(
                std::string(mode == "early_discard" ? "[earlydepthstencil]" : "") +
                    "float4 main(float4 p:SV_Position,uint "
                    "id:SV_PrimitiveID):SV_Target{if(id==0)discard;return float4(p.x,p.y,.25,1);}",
                "ps_5_0");
        if (mode == "inputs") {
            const std::string fields = "struct V{float4 p:SV_Position;float2 a:TEXCOORD0;float "
                                       "b:TEXCOORD1;nointerpolation uint c:TEXCOORD2;};";
            o.shaders[30] = compileClassProgram(
                fields +
                    "V main(uint v:SV_VertexID){uint i=v%3;V "
                    "o;o.p=float4(i==2?3:-1,i==1?3:-1,.25,1);o.a=float2(i,.5);o.b=.25;o.c=v/3;return o;}",
                "vs_5_0");
            o.shaders[32] = compileClassProgram(
                fields + "float4 main(V v,uint id:SV_PrimitiveID,bool front:SV_IsFrontFace):SV_Target{return "
                         "float4(v.a.x,v.b,v.c*.25+id*.125,front?1:0);}",
                "ps_5_0");
        }
        if (mode == "partial_position")
            o.shaders[32] =
                compileClassProgram("float4 main(float4 p:SV_Position):SV_Target{return p.x;}", "ps_5_0");
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto before = o;
        before.before = before.prepareBeforeDraw = true;
        Replay r(f, before);
        r.run();
        const auto originalColor = r.readTexture(20), originalDepth = r.readTexture(720);
        std::vector<std::vector<uint8_t>> colors, depths;
        auto report =
            replayNativePixelGeometry(r, 1000, {0, 2, {1}}, [&](Replay &privateReplay, uint64_t completed) {
                if (completed != colors.size())
                    throw std::runtime_error("Unexpected checkpoint order");
                colors.push_back(privateReplay.readTexture(20));
                depths.push_back(privateReplay.readTexture(720));
            });
        QCOMPARE(colors.size(), size_t(3));
        QCOMPARE(colors[0], originalColor);
        QCOMPARE(depths[0], originalDepth);
        QCOMPARE(r.readTexture(20), originalColor);
        QCOMPARE(r.readTexture(720), originalDepth);
        Json references = Json::array();
        for (uint32_t completed = 1; completed <= 2; ++completed) {
            auto prefix = c;
            drawCount(prefix, 1000, completed * 3);
            const auto p = dir.filePath(QString("prefix%1.gpa_frame").arg(completed));
            prefix.save(p);
            Frame pf(p.toStdWString());
            auto measured = o;
            measured.measurement = ReplayOptions::Measurement{1000, 1000, true};
            Replay reference(pf, measured);
            reference.run();
            QCOMPARE(colors[completed], reference.readTexture(20));
            QCOMPARE(depths[completed], reference.readTexture(720));
            QVERIFY(reference.measurementResult());
            uint64_t cumulative = 0;
            for (uint32_t i = 0; i < completed; ++i)
                cumulative += report.at("batches")[i].at("passing_samples").get<uint64_t>();
            QCOMPARE(cumulative, reference.measurementResult()->occlusion);
            references.push_back({{"completed", completed},
                                  {"passing_samples", reference.measurementResult()->occlusion},
                                  {"color_sha256", sha256(colors[completed])},
                                  {"depth_sha256", sha256(depths[completed])}});
        }
        const auto repeat = replayNativePixelGeometry(r, 1000, {0, 2, {1}});
        QCOMPARE(repeat.at("batches"), report.at("batches"));
        QVERIFY(report.at("original_fragment_eligibility").is_null());
        QCOMPARE(report.at("extra_uav_slots"), Json(0));
        report["reference_prefixes"] = references;
        report["status"] = "ok";
        evidence(report);
    }
    void msaa_data() { devices(); }
    void msaa() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        blendCapture(1, false, false, 4).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.shaders[12] =
            compileClassProgram("float4 main(float4 p:SV_Position,uint "
                                "sample:SV_SampleIndex):SV_Target{return float4(sample*.25,p.xy,.5);}",
                                "ps_5_0");
        o.blendEdits[100].sampleMask = 5;
        Replay reference(f, o);
        reference.run();
        auto samples = [](Replay &p) {
            std::vector<uint8_t> result;
            for (uint32_t sample = 0; sample < 4; ++sample) {
                const auto bytes = p.readMsaa(1000, sample, 2).bytes;
                result.insert(result.end(), bytes.begin(), bytes.end());
            }
            return result;
        };
        const auto expected = samples(reference);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto original = samples(r);
        std::vector<uint8_t> actual;
        auto report = replayNativePixelGeometry(r, 100, {0, 1}, [&](Replay &p, uint64_t n) {
            if (n)
                actual = samples(p);
        });
        QCOMPARE(actual, expected);
        QCOMPARE(samples(r), original);
        report["output_sha256"] = sha256(actual);
        report["status"] = "ok";
        evidence(report);
    }
    void bindings_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"append", "counter", "classes", "predicate_pass", "predicate_skip"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void bindings() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        const bool predicated = mode.startsWith("predicate");
        const bool counter = mode == "append" || mode == "counter";
        auto c = predicated          ? predicateCapture(true, mode == "predicate_skip" ? 1 : 0)
                 : mode == "classes" ? graphicsClassCapture()
                                     : graphicsCounterCapture(mode == "counter");
        const Id event = predicated ? 2000 : 200;
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, event);
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readTexture(20);
        const auto expectedBuffer = counter ? reference.readBuffer(10) : std::vector<uint8_t>{};
        const auto expectedCounter = counter ? reference.readCounter(12) : 0;
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto original = r.readTexture(20);
        const auto originalBuffer = counter ? r.readBuffer(10) : std::vector<uint8_t>{};
        const auto originalCounter = counter ? r.readCounter(12) : 0;
        Com<ID3D11PixelShader> ps;
        std::array<ID3D11ClassInstance *, 256> rawClasses{};
        std::array<Com<ID3D11ClassInstance>, 256> classes;
        UINT classCount = 256;
        context(r)->PSGetShader(&ps, rawClasses.data(), &classCount);
        for (UINT i = 0; i < classCount; ++i)
            classes[i].Attach(rawClasses[i]);
        std::vector<uint8_t> actual, actualBuffer;
        uint32_t actualCounter = 0;
        auto report = replayNativePixelGeometry(
            r, event, {0, mode == "predicate_skip" ? 0u : 1u}, [&](Replay &p, uint64_t) {
                Com<ID3D11PixelShader> bound;
                std::array<ID3D11ClassInstance *, 256> observed{};
                UINT count = 256;
                context(p)->PSGetShader(&bound, observed.data(), &count);
                bool matched = bound == ps && count == classCount;
                for (UINT i = 0; i < count; ++i) {
                    matched &= observed[i] == rawClasses[i];
                    observed[i]->Release();
                }
                if (!matched)
                    throw std::runtime_error("Original PS or class bindings changed");
                actual = p.readTexture(20);
                if (counter) {
                    actualBuffer = p.readBuffer(10);
                    actualCounter = p.readCounter(12);
                }
            });
        QCOMPARE(actual, expected);
        QCOMPARE(r.readTexture(20), original);
        if (counter) {
            QCOMPARE(actualBuffer, expectedBuffer);
            QCOMPARE(actualCounter, expectedCounter);
            QCOMPARE(r.readBuffer(10), originalBuffer);
            QCOMPARE(r.readCounter(12), originalCounter);
            QCOMPARE(actualCounter, originalCounter + 1);
        }
        // Re-execute the original draw on the restored native bindings.
        context(r)->Draw(3, 0);
        QCOMPARE(r.readTexture(20), expected);
        if (counter) {
            QCOMPARE(r.readBuffer(10), expectedBuffer);
            QCOMPARE(r.readCounter(12), expectedCounter);
        }
        report["status"] = "ok";
        report["original_class_count"] = classCount;
        report["counter_before"] = originalCounter;
        report["counter_after_private"] = actualCounter;
        evidence(report);
    }
    void streamOutput_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("dirty");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool dirty : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(dirty))) << vendor << dirty;
    }
    void streamOutput() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, dirty);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        streamCapture(false, dirty).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 150);
        Replay reference(f, o);
        reference.run();
        const auto expected = reference.readBuffer(70);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readBuffer(70);
        auto report = replayNativePixelGeometry(r, 150, {0, 1});
        QCOMPARE(r.readBuffer(70), initial);
        context(r)->Draw(1, 0);
        QCOMPARE(r.readBuffer(70), expected);
        report["status"] = "ok";
        report["restored_so_sha256"] = sha256(expected);
        evidence(report);
    }
    void selections_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"null_ps", "disabled", "empty", "subset"})
                QTest::newRow(qPrintable(QString::number(vendor) + "-" + mode)) << vendor << QString(mode);
    }
    void selections() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        auto c = depthStencilCapture();
        auto o = options(vendor, 1000);
        NativePixelGeometryRange range{0, 1};
        if (mode == "null_ps") {
            c.entries.erase(
                std::remove_if(c.entries.begin(), c.entries.end(), [](const auto &e) { return e.id == 990; }),
                c.entries.end());
            State s{};
            s.topology = 4;
            s.sampleMask = UINT32_MAX;
            s.omStart = 8;
            s.dsv = 722;
            s.viewports = 22;
            s.rasterizer = 724;
            s.stages[0].shader = 30;
            c.add(990, 3, 3, snapshot(s));
        }
        if (mode == "disabled") {
            o.disabled.insert(1000);
            range.primitiveCount = 0;
        }
        if (mode == "empty") {
            drawCount(c, 1000, 0);
            range.primitiveCount = 0;
        }
        if (mode == "subset") {
            drawCount(c, 1000, 6);
            range.firstPrimitive = 1;
            o.shaders[30] = compileClassProgram("float4 main(uint v:SV_VertexID):SV_Position{uint "
                                                "i=v%3;return float4(i==2?3:-1,i==1?3:-1,.25,1);}",
                                                "vs_5_0");
            o.shaders[32] =
                compileClassProgram("float4 main(float4 p:SV_Position,uint "
                                    "id:SV_PrimitiveID):SV_Target{return float4((id+1)*.25,0,0,1);}",
                                    "ps_5_0");
            o.depthStencilEdits[1000].descriptor = depthState();
        }
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        c.save(path);
        Frame f(path.toStdWString());
        Replay reference(f, o);
        reference.run();
        const auto expectedColor = reference.readTexture(20), expectedDepth = reference.readTexture(720);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto beforeColor = r.readTexture(20), beforeDepth = r.readTexture(720);
        std::vector<uint8_t> color, depth;
        auto report = replayNativePixelGeometry(r, 1000, range, [&](Replay &p, uint64_t) {
            color = p.readTexture(20);
            depth = p.readTexture(720);
        });
        QCOMPARE(color, expectedColor);
        QCOMPARE(depth, expectedDepth);
        QCOMPARE(r.readTexture(20), beforeColor);
        QCOMPARE(r.readTexture(720), beforeDepth);
        report["status"] = "ok";
        report["output_sha256"] = sha256(color);
        report["depth_sha256"] = sha256(depth);
        evidence(report);
    }
    void recovery_data() { devices(); }
    void recovery() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("frame.gpa_frame");
        blendCapture().save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor, 100);
        o.before = o.prepareBeforeDraw = true;
        Replay r(f, o);
        r.run();
        const auto initial = r.readTexture(1000);
        bool cancelled = false;
        QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                 replayNativePixelGeometry(
                                     r, 100, {0, 1}, [&](Replay &, uint64_t n) { cancelled = n != 0; },
                                     [&] { return cancelled; }));
        QCOMPARE(r.readTexture(1000), initial);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 replayNativePixelGeometry(r, 100, {0, 1}, [](Replay &, uint64_t) {
                                     throw std::runtime_error("Observer failure");
                                 }));
        QCOMPARE(r.readTexture(1000), initial);
        for (auto selection : std::vector<NativePixelGeometryRange>{{1, 1},
                                                                    {0, 2},
                                                                    {0, 1, {1, 1}},
                                                                    {0, 1, {2}},
                                                                    {0, 1, {}, 1},
                                                                    {0, 1, {}, 256ull * 1024 * 1024, 60001}})
            QVERIFY_THROWS_EXCEPTION(std::exception, replayNativePixelGeometry(r, 100, selection));
        try {
            replayNativePixelGeometry(r, 100, {0, 1, {}, 256ull * 1024 * 1024, 20},
                                      [](Replay &, uint64_t) { QTest::qSleep(30); });
            QFAIL("Expired diagnostic deadline was accepted");
        } catch (const OperationCancelled &) {
            QFAIL("Deadline was reported as user cancellation");
        } catch (const std::runtime_error &error) {
            QCOMPARE(std::string(error.what()), std::string("Native pixel geometry replay deadline"));
        }
        std::vector<uint8_t> result;
        auto report = replayNativePixelGeometry(r, 100, {0, 1}, [&](Replay &p, uint64_t n) {
            if (n)
                result = p.readTexture(1000);
        });
        QCOMPARE(r.readTexture(1000), initial);
        o.before = o.prepareBeforeDraw = false;
        Replay reference(f, o);
        reference.run();
        QCOMPARE(result, reference.readTexture(1000));
        report["status"] = "ok";
        report["recovered_failures"] = 9;
        evidence(report);
    }
};
QTEST_GUILESS_MAIN(NativePixelGeometryReplayTests)
#include "NativePixelGeometryReplayTests.moc"
