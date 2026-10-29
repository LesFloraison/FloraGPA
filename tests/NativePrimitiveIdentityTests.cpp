#include "ClassCapture.h"
#include "HullCapture.h"
#include "StreamCapture.h"
#include "application/DxbcGpuCompletion.h"
#include "application/DxbcPixelGeometry.h"
#include "application/NativePixelCandidates.h"
#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <d3d11sdklayers.h>
#include <set>
using namespace flora;
using namespace flora::testing;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
const std::string pixelSource =
    "uint main(float4 p:SV_Position,uint id:SV_PrimitiveID):SV_Target{return id+1;}";
void debugErrors(Replay &r) {
    Com<ID3D11InfoQueue> queue;
    check(r.nativeDevice()->QueryInterface(IID_PPV_ARGS(&queue)), "Get identity debug messages");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Size identity debug message");
        std::vector<uint8_t> data(size);
        auto *m = reinterpret_cast<D3D11_MESSAGE *>(data.data());
        check(queue->GetMessage(i, m, &size), "Read identity debug message");
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(m->pDescription);
    }
}
void shader(Capture &c, Id id, uint16_t type, const std::string &text, const char *profile) {
    const auto code = compileClassProgram(text, profile);
    c.add(id, 5, type, statePack(Id(0), Id(0), Id(0), Id(0), Id(0), Id(0), id + 1));
    auto raw = statePack(uint64_t(code.size()));
    raw.insert(raw.end(), code.begin(), code.end());
    append(raw, Id(0));
    c.add(id + 1, 9, 0x81, raw);
}
void buffer(Capture &c, Id id, UINT bind, UINT misc, const std::vector<uint32_t> &values) {
    auto raw = statePack(Id(0), Id(0), uint32_t(values.size() * 4), 0u, bind, 0u, misc, 0u, id + 1);
    c.add(id, 5, 0x83, raw);
    raw = word(uint32_t(values.size() * 4));
    for (auto value : values)
        append(raw, value);
    c.add(id + 1, 9, 1, raw);
}
Capture capture(uint32_t topology, uint32_t vertices, uint32_t instances, uint16_t draw = 0x3c,
                bool gs = false) {
    Capture c;
    c.add(1, 5, 0x127, statePack(Id(0), Id(0), 0u, 0u));
    shader(c, 10, 0x90,
           "float4 main(uint v:SV_VertexID,uint inst:SV_InstanceID):SV_Position{"
           "uint cell=v/3;uint corner=v%3;float2 p=float2(2+cell*12,2+(inst%7)*12);"
           "p+=float2(corner==2?8:0,corner==1?8:0);return float4(p.x/64-1,1-p.y/64,.5,1);}",
           "vs_5_0");
    shader(c, 12, 0x92, pixelSource, "ps_5_0");
    if (gs)
        shader(c, 14, 0x91,
               "struct V{float4 p:SV_Position;};struct O{float4 p:SV_Position;uint id:SV_PrimitiveID;};"
               "[maxvertexcount(3)]void main(triangle V v[3],uint id:SV_PrimitiveID,"
               "inout TriangleStream<O> output){[unroll]for(uint i=0;i<3;i++){O o;"
               "o.p=v[i].p;o.id=100+id*7+i;output.Append(o);}}",
               "gs_5_0");
    c.add(20, 5, 0x85, statePack(Id(0), Id(0), 128u, 128u, 1u, 1u, 42u, 1u, 0u, 0u, 32u, 0u, 0u, Id(0)));
    c.add(22, 5, 0x8d, statePack(Id(0), Id(0), Id(20), 42u, 4u, 0u, 0u, 0u));
    c.add(24, 9, 0x87, statePack(1u, 0.f, 0.f, 128.f, 128.f, 0.f, 1.f));
    c.add(26, 5, 0x89, statePack(Id(0), Id(0), 3u, 1u, 0u, 0, 0.f, 0.f, 1u, 0u, 0u, 0u));
    State s{};
    s.topology = topology;
    s.sampleMask = UINT32_MAX;
    s.omStart = 8;
    s.rtCount = 1;
    s.rtv[0] = 22;
    s.rasterizer = 26;
    s.viewports = 24;
    s.stages[0].shader = 10;
    s.stages[4].shader = 12;
    s.stages[3].shader = gs ? 14 : 0;
    const bool indexed = draw == 0x39 || draw == 0x3a || draw == 0x3b;
    if (indexed) {
        // Two prefix entries exercise both the binding offset and start index.
        // Reverse primitive order independently from the system primitive IDs.
        std::vector<uint32_t> indices{999, 999};
        for (uint32_t i = 0; i < vertices; ++i)
            indices.push_back((vertices / 3 - 1 - i / 3) * 3 + i % 3);
        buffer(c, 30, D3D11_BIND_INDEX_BUFFER, 0, indices);
        s.ib = 30;
        s.ibFormat = 42;
        s.ibOffset = 4;
    }
    c.add(90, 3, 3, snapshot(s));
    auto raw = statePack(Id(90), Id(0), Id(1));
    std::vector<uint32_t> args;
    if (draw == 0x37)
        args = {vertices, 3};
    if (draw == 0x39)
        args = {vertices, 1, 3};
    if (draw == 0x3a || draw == 0x3b)
        args = {vertices, instances, 1, 3, 2};
    if (draw == 0x3c || draw == 0x3d)
        args = {vertices, instances, 3, 2};
    if (draw == 0x3b || draw == 0x3d) {
        args.insert(args.begin(), 0xabcdef01);
        buffer(c, 40, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, args);
        append(raw, Id(40));
        append(raw, 4u);
    } else
        for (auto value : args)
            append(raw, value);
    c.add(100, 7, draw, raw);
    return c;
}
ReplayOptions options(uint32_t vendor, Id event = 100) {
    ReplayOptions o;
    o.warp = !vendor;
    if (vendor)
        o.vendor = vendor;
    o.before = o.prepareBeforeDraw = true;
    o.until = event;
    o.debug = true;
    return o;
}
void evidence(const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PRIMITIVE_ID_EVIDENCE");
    if (root.isEmpty())
        return;
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create primitive identity evidence directory");
    const auto name = QString::fromLatin1(QTest::currentTestFunction()) + "-" +
                      QString::fromLatin1(QTest::currentDataTag()) + ".json";
    QFile file(QDir(root).filePath(name));
    const auto text = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save primitive identity evidence");
}
} // namespace
class NativePrimitiveIdentityTests final : public QObject {
    Q_OBJECT
  private slots:
    void topology_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("topology");
        QTest::addColumn<uint32_t>("count");
        QTest::addColumn<uint32_t>("instances");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (auto [topology, count] : std::vector<std::pair<uint32_t, uint32_t>>{
                     {1, 24}, {2, 12}, {3, 23}, {4, 8}, {5, 22}, {10, 6}, {11, 21}, {12, 4}, {13, 10}})
                for (uint32_t instances : {1u, 2u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(topology).arg(instances)))
                        << vendor << topology << count << instances;
    }
    void topology() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, topology);
        QFETCH(uint32_t, count);
        QFETCH(uint32_t, instances);
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        capture(topology, 24, instances).save(path);
        Frame f(path.toStdWString());
        Replay r(f, options(vendor));
        r.run();
        const auto before = r.readTexture(20);
        const auto g = inspectPixelGeometry(r, 100);
        const auto ids = validatedPixelPrimitiveIds(g);
        const auto mapping = validatedPixelGeometryInstances(g);
        QVERIFY(mapping.resolved);
        QCOMPARE(ids.size(), size_t(count * instances));
        for (uint32_t i = 0; i < ids.size(); ++i) {
            QCOMPARE(ids[i], i % count);
            QCOMPARE(mapping.instance(i), std::optional<uint32_t>(i / count));
        }
        QCOMPARE(r.readTexture(20), before);
        QCOMPARE(inspectPixelGeometry(r, 100).primitiveIds, g.primitiveIds);
        debugErrors(r);
        evidence({{"geometry", g.report},
                  {"ids", g.primitiveIds},
                  {"expected_per_instance", count},
                  {"instances", instances},
                  {"status", "ok"}});
    }
    void originalPixel_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("draw");
        QTest::addColumn<bool>("gs");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t draw : {0x37u, 0x39u, 0x3au, 0x3bu, 0x3cu, 0x3du})
                for (bool gs : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(draw).arg(gs)))
                        << vendor << draw << gs;
    }
    void originalPixel() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, draw);
            QFETCH(bool, gs);
            QTemporaryDir dir;
            const auto path = dir.filePath("fixture.gpa_frame");
            capture(4, 12, 2, uint16_t(draw), gs).save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor);
            Replay r(f, o);
            r.run();
            const auto before = r.readTexture(20);
            const auto g = inspectPixelGeometry(r, 100);
            QCOMPARE(r.readTexture(20), before);
            o.before = o.prepareBeforeDraw = false;
            Replay full(f, o);
            full.run();
            const auto pixels = full.readTexture(20);
            debugErrors(full);
            const auto stride = g.report.at("stride").get<size_t>();
            size_t position = SIZE_MAX;
            for (const auto &field : g.report.at("attributes"))
                if (field.at("system_value") == 1)
                    position = field.at("offset").get<size_t>();
            QVERIFY(position != SIZE_MAX);
            Json observations = Json::array();
            const auto ids = validatedPixelPrimitiveIds(g);
            const auto mapping = validatedPixelGeometryInstances(g);
            QVERIFY(mapping.resolved);
            QCOMPARE(ids.size(), size_t(draw == 0x37 || draw == 0x39 ? 4 : 8));
            for (size_t i = 0; i < ids.size(); ++i) {
                QCOMPARE(mapping.instance(i), std::optional<uint32_t>(uint32_t(i / 4)));
                float x = 0, y = 0;
                for (size_t v = 0; v < 3; ++v) {
                    Reader input(Bytes(g.bytes).subspan((i * 3 + v) * stride + position, 16));
                    x += input.read<float>();
                    y += input.read<float>();
                }
                const auto px = uint32_t((x / 3 + 1) * 64), py = uint32_t((1 - y / 3) * 64);
                QVERIFY(px < 128 && py < 128);
                const auto observed = Reader(Bytes(pixels).subspan((py * 128 + px) * 4, 4)).read<uint32_t>();
                QCOMPARE(observed, ids[i] + 1);
                QCOMPARE(ids[i], gs ? 100u + uint32_t(i % 4) * 7 : uint32_t(i % 4));
                observations.push_back(
                    {{"ordinal", i}, {"x", px}, {"y", py}, {"original_ps_value", observed}});
            }
            // Keep the original position signature so the GS and PS hardware registers agree.
            const auto ps = compileClassProgram(pixelSource, "ps_5_0");
            PixelRasterState raster;
            r.inspectNativeState([&](auto *context, const auto &) { raster = pixelRasterState(context); });
            PixelInputRecordSelection selection;
            selection.x = observations[0].at("x");
            selection.y = observations[0].at("y");
            const auto helper = pixelInputShader(ps, 1, selection);
            const auto bridge = pixelGeometryRangeBridge(g, helper, 0, ids.size());
            QCOMPARE(bridge.identity.at("version"), Json(3));
            const auto input = capturePixelRasterInputs(r.nativeDevice(), !vendor, raster, bridge, helper);
            QCOMPARE(input.records.size(), size_t(1));
            const auto &words = input.records[0].words;
            const auto lane = words[4][2] + 2 * words[4][3];
            QCOMPARE(words[lane][0], ids[0]);
            NativePixelCandidateSelection candidateSelection;
            candidateSelection.primitiveCount = ids.size();
            candidateSelection.x = selection.x;
            candidateSelection.y = selection.y;
            const auto candidates =
                captureNativePixelCandidates(r.nativeDevice(), !vendor, raster, g, candidateSelection);
            QCOMPARE(candidates.report.at("observations").size(), size_t(1));
            QCOMPARE(candidates.report.at("observations")[0].at("original_primitive_id"), Json(ids[0]));
            QCOMPARE(candidates.report.at("observations")[0].at("geometry_instance"), Json(0));
            candidateSelection.x = observations.back().at("x");
            candidateSelection.y = observations.back().at("y");
            const auto lastCandidates =
                captureNativePixelCandidates(r.nativeDevice(), !vendor, raster, g, candidateSelection);
            QCOMPARE(lastCandidates.report.at("observations").size(), size_t(1));
            QCOMPARE(lastCandidates.report.at("observations")[0].at("geometry_instance"),
                     Json(draw == 0x37 || draw == 0x39 ? 0 : 1));
            for (const auto *key :
                 {"geometry_sha256", "primitive_count", "domain", "original_gpu_schedule"}) {
                auto corrupt = g;
                corrupt.report["geometry_instances"][key] = "invalid";
                QVERIFY_THROWS_EXCEPTION(std::exception, validatedPixelGeometryInstances(corrupt));
            }
            QVERIFY_THROWS_EXCEPTION(std::invalid_argument, mapping.instance(ids.size()));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryBridge(g, helper, 0, 123u));
            auto bad = g;
            bad.bytes[0] ^= 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validatedPixelPrimitiveIds(bad));
            bad = g;
            bad.primitiveIds[0] ^= 1;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validatedPixelPrimitiveIds(bad));
            debugErrors(r);
            debugErrors(full);
            evidence({{"geometry", g.report},
                      {"ids", g.primitiveIds},
                      {"original_pixels", observations},
                      {"bridge", bridge.identity},
                      {"bridge_observed_id", words[lane][0]},
                      {"candidates", candidates.report},
                      {"status", "ok"}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void unknown_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t v : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(v))) << v;
    }
    void tessellation_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("domain");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const QString domain : {QString("tri"), QString("quad"), QString("isoline")})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(domain))) << vendor << domain;
    }
    void tessellation() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(QString, domain);
            const auto cp = domain == "tri" ? 3u : domain == "quad" ? 4u : 2u;
            const auto n = std::to_string(cp), d = domain.toStdString();
            auto c = capture(32 + cp, cp * 2, 2);
            // Use quadrilateral line coverage on every adapter. The standalone
            // probe separately records the WARP legacy-line rendering failure.
            if (cp == 2)
                for (const auto &entry : c.entries)
                    if (entry.id == 26)
                        put(c.bytes, entry.offset + 48, 1u);
            State state{};
            state.topology = 32 + cp;
            state.sampleMask = UINT32_MAX;
            state.omStart = 8;
            state.rtCount = 1;
            state.rtv[0] = 22;
            state.rasterizer = 26;
            state.viewports = 24;
            state.stages[0].shader = 10;
            state.stages[4].shader = 12;
            c.entries.erase(
                std::remove_if(c.entries.begin(), c.entries.end(),
                               [](const auto &e) { return e.id == 90 || e.id == 10 || e.id == 11; }),
                c.entries.end());
            shader(c, 10, 0x90,
                   "float4 main(uint v:SV_VertexID,uint inst:SV_InstanceID):SV_Position{"
                   "uint cell=v/" +
                       n + ";uint corner=v%" + n +
                       ";float2 p=float2(2.25+cell*24,2.25+(inst%7)*12);"
                       "p+=float2(" +
                       std::string(cp == 2 ? "corner==1?8:0" : "corner>=2?8:0") + "," +
                       std::string(cp == 2 ? "0" : "corner%2==1?8:0") +
                       ");return float4(p.x/64-1,1-p.y/64,.5,1);}",
                   "vs_5_0");
            const std::string v = "struct V{float4 p:SV_Position;};";
            const std::string constants =
                cp == 2   ? "struct C{float e[2]:SV_TessFactor;};"
                : cp == 3 ? "struct C{float e[3]:SV_TessFactor;float i:SV_InsideTessFactor;};"
                          : "struct C{float e[4]:SV_TessFactor;float i[2]:SV_InsideTessFactor;};";
            const std::string assign = cp == 2   ? "c.e[0]=c.e[1]=2;"
                                       : cp == 3 ? "c.e[0]=c.e[1]=c.e[2]=c.i=2;"
                                                 : "c.e[0]=c.e[1]=c.e[2]=c.e[3]=c.i[0]=c.i[1]=2;";
            shader(c, 50, 0x95,
                   v + constants + "C patch(InputPatch<V," + n + "> p){C c;" + assign +
                       "return c;}"
                       "[domain(\"" +
                       d + "\")][partitioning(\"integer\")][outputtopology(\"" +
                       std::string(cp == 2 ? "line" : "triangle_cw") +
                       "\")]"
                       "[outputcontrolpoints(" +
                       n +
                       ")][patchconstantfunc(\"patch\")]"
                       "V main(InputPatch<V," +
                       n + "> p,uint id:SV_OutputControlPointID){return p[id];}",
                   "hs_5_0");
            shader(c, 52, 0x94,
                   v + constants + "[domain(\"" + d + "\")]float4 main(C c,float" +
                       std::string(cp == 3 ? "3" : "2") + " loc:SV_DomainLocation,const OutputPatch<V," + n +
                       "> "
                       "p):SV_Position"
                       "{return " +
                       std::string(cp == 3 ? "p[0].p*loc.x+p[1].p*loc.y+p[2].p*loc.z"
                                   : cp == 4
                                       ? "lerp(lerp(p[0].p,p[2].p,loc.x),lerp(p[1].p,p[3].p,loc.x),loc.y)"
                                       : "lerp(p[0].p,p[1].p,loc.x)+float4(0,-loc.y*6/64,0,0)") +
                       ";}",
                   "ds_5_0");
            state.stages[1].shader = 50;
            state.stages[2].shader = 52;
            c.add(90, 3, 3, snapshot(state));
            QTemporaryDir dir;
            const auto path = dir.filePath("tess.gpa_frame");
            c.save(path);
            Frame f(path.toStdWString());
            auto o = options(vendor);
            Replay r(f, o);
            r.run();
            const auto g = inspectPixelGeometry(r, 100);
            const auto ids = validatedPixelPrimitiveIds(g);
            QVERIFY(!ids.empty());
            o.before = o.prepareBeforeDraw = false;
            Replay full(f, o);
            Com<ID3D11Query> query;
            D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            check(full.nativeDevice()->CreateQuery(&qd, &query), "Create tessellation reference query");
            Com<ID3D11DeviceContext> context;
            full.nativeDevice()->GetImmediateContext(&context);
            context->Begin(query.Get());
            full.run();
            context->End(query.Get());
            GpuCompletion complete(full.nativeDevice(), context.Get());
            complete.submit();
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            complete.query(query.Get(), &stats, sizeof stats);
            QCOMPARE(ids.size(), size_t(stats.CPrimitives));
            debugErrors(full);

            const auto pixels = full.readTexture(20);
            const auto stride = g.report.at("stride").get<size_t>();
            const auto factor = g.report.at("vertices_per_primitive").get<size_t>();
            Json observed = Json::array();
            for (size_t i = 0; i < ids.size(); ++i) {
                float x = 0, y = 0;
                for (size_t v = 0; v < factor; ++v) {
                    Reader input(Bytes(g.bytes).subspan((i * factor + v) * stride, 16));
                    x += input.read<float>();
                    y += input.read<float>();
                }
                const auto px = uint32_t((x / factor + 1) * 64), py = uint32_t((1 - y / factor) * 64);
                QVERIFY(px < 128 && py < 128);
                const auto value = Reader(Bytes(pixels).subspan((py * 128 + px) * 4, 4)).read<uint32_t>();
                QCOMPARE(value, ids[i] + 1);
                observed.push_back({{"ordinal", i}, {"original_ps_value", value}, {"x", px}, {"y", py}});
            }
            debugErrors(r);
            debugErrors(full);
            QCOMPARE((std::set<uint32_t>(ids.begin(), ids.end())), (std::set<uint32_t>{0, 1}));
            evidence({{"geometry", g.report},
                      {"ids", g.primitiveIds},
                      {"original_pixels", observed},
                      {"reference_clipper_primitives", stats.CPrimitives},
                      {"status", "ok"}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void unknown() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        Json rows = Json::array();
        for (int mode : {0, 1, 2}) {
            const bool gs = mode == 1;
            const auto path = dir.filePath(gs ? "gs.gpa_frame" : "hs.gpa_frame");
            auto c = gs ? streamCapture() : hullCapture();
            if (mode == 2) {
                const auto entry = *std::find_if(c.entries.begin(), c.entries.end(),
                                                 [](const auto &e) { return e.id == 7003; });
                Reader r(Bytes(c.bytes).subspan(entry.offset, entry.size));
                const auto size = r.read<uint64_t>();
                const auto original = r.take(size);
                Com<ID3DBlob> stripped;
                check(D3DStripShader(original.data(), original.size(), D3DCOMPILER_STRIP_REFLECTION_DATA,
                                     &stripped),
                      "Strip HS reflection");
                auto raw = statePack(uint64_t(stripped->GetBufferSize()));
                const auto *begin = static_cast<const uint8_t *>(stripped->GetBufferPointer());
                raw.insert(raw.end(), begin, begin + stripped->GetBufferSize());
                append(raw, Id(0));
                c.entries.erase(std::remove_if(c.entries.begin(), c.entries.end(),
                                               [](const auto &e) { return e.id == 7003; }),
                                c.entries.end());
                c.add(7003, 9, 0x81, raw);
            }
            c.save(path);
            Frame f(path.toStdWString());
            Replay r(f, options(vendor, gs ? 150 : 200));
            r.run();
            const auto before = r.readBuffer(gs ? 70 : 10);
            const auto counter = gs ? 0 : r.readCounter(12);
            const auto g = inspectPixelGeometry(r, gs ? 150 : 200);
            QCOMPARE(g.report.at("primitive_identity").at("status"), Json("unavailable"));
            QVERIFY(validatedPixelPrimitiveIds(g).empty());
            QCOMPARE(r.readBuffer(gs ? 70 : 10), before);
            if (!gs)
                QCOMPARE(r.readCounter(12), counter);
            rows.push_back(g.report);
        }
        evidence({{"cases", rows}, {"status", "ok"}});
    }
    void boundaries_data() { unknown_data(); }
    void streamOutput_data() { unknown_data(); }
    void streamOutput() {
        try {
            QFETCH(uint32_t, vendor);
            QTemporaryDir dir;
            Json rows = Json::array();
            for (bool provider : {false, true}) {
                const auto path = dir.filePath(provider ? "provider.gpa_frame" : "auto.gpa_frame");
                streamCapture(provider).save(path);
                Frame f(path.toStdWString());
                const Id event = provider ? 150 : 200;
                auto o = options(vendor, event);
                Replay r(f, o);
                r.run();
                const auto bytes = r.readBuffer(70);
                const auto pixels = r.readTexture(20);
                const auto g = inspectPixelGeometry(r, event);
                QCOMPARE(g.primitiveIds, (provider ? std::vector<uint32_t>{0} : std::vector<uint32_t>{0, 1}));
                QCOMPARE(validatedPixelPrimitiveIds(g).size(), size_t(provider ? 1 : 2));
                QCOMPARE(r.readBuffer(70), bytes);
                QCOMPARE(r.readTexture(20), pixels);
                r.inspectNativeState([&](auto *context, const auto &) {
                    if (provider)
                        context->Draw(3, 0);
                    else
                        context->DrawAuto();
                });
                o.before = o.prepareBeforeDraw = false;
                Replay clean(f, o);
                clean.run();
                QCOMPARE(r.readBuffer(70), clean.readBuffer(70));
                QCOMPARE(r.readTexture(20), clean.readTexture(20));
                debugErrors(r);
                debugErrors(clean);
                rows.push_back(
                    {{"geometry", g.report}, {"ids", g.primitiveIds}, {"signature_provider", provider}});
            }
            evidence({{"cases", rows}, {"status", "ok"}});
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void boundaries() {
        QFETCH(uint32_t, vendor);
        QTemporaryDir dir;
        const auto path = dir.filePath("fixture.gpa_frame");
        capture(4, 12, 2).save(path);
        Frame f(path.toStdWString());
        auto o = options(vendor);
        Replay r(f, o);
        r.run();
        const auto before = r.readTexture(20);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, inspectPixelGeometry(r, 100, 0, 8));
        QCOMPARE(r.readTexture(20), before);
        const auto g = inspectPixelGeometry(r, 100);
        for (int fault = 0; fault < 9; ++fault) {
            auto bad = g;
            switch (fault) {
            case 0:
                bad.report.erase("primitive_identity");
                break;
            case 1:
                bad.report["primitive_identity"]["status"] = "unavailable";
                break;
            case 2:
                bad.report["primitive_identity"]["schema"] = 2;
                break;
            case 3:
                bad.report["primitive_identity"]["primitive_count"] = 999;
                break;
            case 4:
                bad.report["primitive_identity"]["pipeline_order"] = "atomic_record_order";
                break;
            case 5:
                bad.report["primitive_identity"]["original_gpu_schedule"] = true;
                break;
            case 6:
                bad.report["primitive_identity"]["source"] = "inferred_ordinal";
                break;
            case 7:
                bad.primitiveIds.pop_back();
                break;
            case 8:
                bad.bytes[0] ^= 1;
                break;
            }
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, validatedPixelPrimitiveIds(bad));
        }
        o.suppressDraws = true;
        Replay disabled(f, o);
        disabled.run();
        const auto empty = inspectPixelGeometry(disabled, 100);
        QCOMPARE(empty.report.at("primitive_identity").at("status"), Json("observed"));
        QVERIFY(validatedPixelPrimitiveIds(empty).empty());
        QCOMPARE(empty.report.at("primitives"), Json(0));
        debugErrors(r);
        debugErrors(disabled);
        evidence(
            {{"faults_rejected", 9}, {"budget_rejected", true}, {"empty", empty.report}, {"status", "ok"}});
    }
};
QTEST_GUILESS_MAIN(NativePrimitiveIdentityTests)
#include "NativePrimitiveIdentityTests.moc"
