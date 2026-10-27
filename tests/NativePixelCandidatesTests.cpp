#include "application/DxbcGpuCompletion.h"
#include "application/DxbcPixelTraceInputs.h"
#include "application/HlslCompilation.h"
#include "application/NativePixelCandidates.h"
#include "replay/Device.h"
#include <QDir>
#include <QFile>
#include <QtTest>
#include <bit>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
void devices() {
    QTest::addColumn<uint32_t>("vendor");
    for (uint32_t vendor : {0u, 4318u, 32902u})
        QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
}
PostTransformGeometry geometry() {
    PostTransformGeometry g;
    g.report = {{"geometry_stage", "final_pre_rasterization"},
                {"stride", 24},
                {"vertices_per_primitive", 3},
                {"primitives", 6},
                {"vertices", 18},
                {"attributes", Json::array()}};
    for (const auto &[name, system, type, offset, count] :
         std::vector<std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t>>{
             {"SV_Position", 1, 3, 0, 4},
             {"SV_ClipDistance", 2, 3, 16, 1},
             {"SV_RenderTargetArrayIndex", 4, 1, 20, 1}}) {
        std::vector<uint32_t> components;
        for (uint32_t i = 0; i < count; ++i)
            components.push_back(i);
        g.report["attributes"].push_back({{"semantic", name},
                                          {"index", 0},
                                          {"system_value", system},
                                          {"component_type", type},
                                          {"offset", offset},
                                          {"component_count", count},
                                          {"components", components}});
    }
    for (uint32_t primitive = 0; primitive < 6; ++primitive)
        for (uint32_t vertex = 0; vertex < 3; ++vertex) {
            const float x = primitive == 2 ? 4.f : primitive == 5 ? -1.f : vertex == 2 ? 3.f : -1.f;
            const float y = primitive == 5 ? -1.f : vertex == 1 ? 3.f : -1.f;
            const std::array<uint32_t, 6> v{std::bit_cast<uint32_t>(x),
                                            std::bit_cast<uint32_t>(y),
                                            std::bit_cast<uint32_t>(.5f),
                                            std::bit_cast<uint32_t>(1.f),
                                            std::bit_cast<uint32_t>(primitive == 3 ? -1.f : 1.f),
                                            primitive == 4 ? 1u : 0u};
            const auto *bytes = reinterpret_cast<const uint8_t *>(v.data());
            g.bytes.insert(g.bytes.end(), bytes, bytes + sizeof(v));
        }
    return g;
}
PixelRasterState raster(uint32_t samples = 1) {
    PixelRasterState s;
    s.target = {D3D11_RESOURCE_DIMENSION_TEXTURE2D, 8, 8, 2, DXGI_FORMAT_R8G8B8A8_UNORM, {samples, 0}, false};
    s.rasterizer.FillMode = D3D11_FILL_SOLID;
    s.rasterizer.CullMode = D3D11_CULL_NONE;
    s.rasterizer.DepthClipEnable = TRUE;
    s.rasterizer.MultisampleEnable = samples > 1;
    s.viewports = {{0, 0, 8, 8, 0, 1}};
    return s;
}
void validate(ID3D11Device *device) {
    Com<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)), "Query candidate debug queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Size candidate debug message");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read candidate debug message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}
// Independent normal D3D11 draw: compiled VS generates positions/clip distances,
// the PS rejects all pixels except the selection, and a query observes coverage.
// It does not consume bridge shaders, tagged geometry, UAV records or derivatives.
uint64_t referenceCount(Dx11Device &device, const PixelRasterState &state,
                        const NativePixelCandidateSelection &selection, uint32_t primitive) {
    auto *d = device.device.Get();
    auto *c = device.context.Get();
    c->ClearState();
    const std::string source = "struct V{float4 p:SV_Position;float clip:SV_ClipDistance;};"
                               "V main(uint v:SV_VertexID){V o;uint n=" +
                               std::to_string(primitive) +
                               ";"
                               "o.p=float4(n==2?4:n==5?-1:v==2?3:-1,n==5?-1:v==1?3:-1,.5,1);"
                               "o.clip=n==3?-1:1;return o;}";
    const auto vs = compileHlsl(source, "vs_5_0").bytecode;
    const auto ps = compileHlsl("float4 main(float4 p:SV_Position):SV_Target{"
                                "if(any(int2(p.xy)!=int2(" +
                                    std::to_string(selection.x) + "," + std::to_string(selection.y) +
                                    ")))discard;return 1;}",
                                "ps_5_0")
                        .bytecode;
    Com<ID3D11VertexShader> vertex;
    Com<ID3D11PixelShader> pixel;
    check(d->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex), "Create independent candidate VS");
    check(d->CreatePixelShader(ps.data(), ps.size(), nullptr, &pixel), "Create independent candidate PS");
    D3D11_TEXTURE2D_DESC td{8,
                            8,
                            1,
                            1,
                            DXGI_FORMAT_R8G8B8A8_UNORM,
                            state.target.samples,
                            D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET,
                            0,
                            0};
    Com<ID3D11Texture2D> texture;
    Com<ID3D11RenderTargetView> view;
    check(d->CreateTexture2D(&td, nullptr, &texture), "Create independent candidate target");
    check(d->CreateRenderTargetView(texture.Get(), nullptr, &view), "Create independent candidate view");
    D3D11_RASTERIZER_DESC rd{};
    std::memcpy(&rd, &state.rasterizer, sizeof(rd));
    Com<ID3D11RasterizerState> rs;
    check(d->CreateRasterizerState(&rd, &rs), "Create independent candidate rasterizer");
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                  D3D11_COMPARISON_ALWAYS};
    Com<ID3D11DepthStencilState> ds;
    check(d->CreateDepthStencilState(&dd, &ds), "Create independent candidate depth state");
    c->RSSetState(rs.Get());
    c->RSSetViewports(UINT(state.viewports.size()), state.viewports.data());
    c->RSSetScissorRects(UINT(state.scissors.size()), state.scissors.data());
    c->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
    c->OMSetDepthStencilState(ds.Get(), 0);
    c->OMSetBlendState(nullptr, nullptr, state.sampleMask & (1u << selection.sample));
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex.Get(), nullptr, 0);
    c->PSSetShader(pixel.Get(), nullptr, 0);
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    Com<ID3D11Query> query;
    check(d->CreateQuery(&qd, &query), "Create independent candidate query");
    c->Begin(query.Get());
    if ((primitive == 4 ? 1u : 0u) == selection.layer)
        c->Draw(3, 0);
    c->End(query.Get());
    GpuCompletion completion(d, c);
    completion.submit();
    uint64_t count = 0;
    completion.query(query.Get(), &count, sizeof(count));
    validate(d);
    c->ClearState();
    return count;
}
void evidence(const std::string &name, const Json &report) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_CANDIDATE_EVIDENCE");
    if (root.isEmpty())
        return;
    QDir directory(root);
    if (!directory.exists() && !QDir().mkpath(root))
        throw std::runtime_error("Cannot create candidate evidence directory");
    QFile file(directory.filePath(QString::fromStdString(name) + ".json"));
    const auto text = report.dump(2);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save candidate evidence");
}
} // namespace
class NativePixelCandidatesTests final : public QObject {
    Q_OBJECT
  private slots:
    void coverage_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("layer");
        QTest::addColumn<uint32_t>("sample");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 4u})
                for (uint32_t layer : {0u, 1u})
                    for (uint32_t sample = 0; sample < samples; ++sample)
                        for (const auto &mode :
                             {QString("ordinary"), QString("scissor"), QString("mask"), QString("range")})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(samples)
                                                         .arg(layer)
                                                         .arg(sample)
                                                         .arg(mode)))
                                << vendor << samples << layer << sample << mode;
    }
    void coverage() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, samples);
        QFETCH(uint32_t, layer);
        QFETCH(uint32_t, sample);
        QFETCH(QString, mode);
        auto device = createDx11Device(!vendor, true, vendor ? std::optional(vendor) : std::nullopt);
        auto g = geometry();
        auto s = raster(samples);
        NativePixelCandidateSelection selection;
        selection.x = selection.y = 4;
        selection.layer = layer;
        selection.sample = sample;
        selection.firstPrimitive = mode == "range" ? 1 : 0;
        selection.primitiveCount = mode == "range" ? 4 : 6;
        if (mode == "scissor") {
            s.rasterizer.ScissorEnable = TRUE;
            s.scissors = {{0, 0, 2, 2}};
        }
        if (mode == "mask")
            s.sampleMask = ~(1u << sample);
        const auto result = captureNativePixelCandidates(device.device.Get(), !vendor, s, g, selection);
        auto expected = Json::array();
        auto references = Json::array();
        for (uint32_t i = uint32_t(selection.firstPrimitive);
             i < selection.firstPrimitive + selection.primitiveCount; ++i) {
            const auto count = referenceCount(device, s, selection, i);
            references.push_back({{"ordinal", i}, {"count", count}});
            const bool covers =
                mode != "scissor" && mode != "mask" && (layer == 0 ? (i == 0 || i == 1) : i == 4);
            QCOMPARE(count, uint64_t(covers));
            if (covers)
                expected.push_back(i);
        }
        QCOMPARE(result.report.at("unique_primitive_ordinals"), expected);
        QCOMPARE(result.observations.records.size(), expected.size());
        QCOMPARE(result.report.at("raster_passes"), Json(1));
        QVERIFY(result.report.at("original_fragment_eligibility").is_null());
        QCOMPARE(result.observations.identity.at("scope"), Json("isolated_geometry_range_pre_depth_inputs"));
        auto report = result.report;
        report["reference_counts"] = references;
        report["adapter"] = device.information();
        evidence(QTest::currentDataTag(), report);
        validate(device.device.Get());
    }
    void boundsAndRetry_data() { devices(); }
    void boundsAndRetry() {
        QFETCH(uint32_t, vendor);
        auto d = createDx11Device(!vendor, true, vendor ? std::optional(vendor) : std::nullopt);
        auto g = geometry();
        auto s = raster();
        NativePixelCandidateSelection selection;
        selection.x = selection.y = 4;
        selection.primitiveCount = 6;
        auto run = [&] { return captureNativePixelCandidates(d.device.Get(), !vendor, s, g, selection); };
        QVERIFY_EXCEPTION_THROWN(
            captureNativePixelCandidates(d.device.Get(), !vendor, s, g, selection, [] { return true; }),
            OperationCancelled);
        selection.maxRecords = 1;
        QVERIFY_EXCEPTION_THROWN(run(), std::runtime_error);
        selection.maxRecords = 4096;
        selection.maxBytes = 1;
        QVERIFY_EXCEPTION_THROWN(run(), std::runtime_error);
        selection.maxBytes = 256ull * 1024 * 1024;
        selection.primitiveCount = 0;
        QVERIFY_EXCEPTION_THROWN(run(), std::invalid_argument);
        selection.primitiveCount = 6;
        selection.firstPrimitive = 1;
        QVERIFY_EXCEPTION_THROWN(run(), std::invalid_argument);
        selection.firstPrimitive = 0;
        g.report["attributes"][2]["offset"] = 24;
        QVERIFY_EXCEPTION_THROWN(run(), std::runtime_error);
        g = geometry();
        g.report["attributes"][2]["semantic"] = "flora_candidate_ordinal";
        QVERIFY_EXCEPTION_THROWN(run(), std::runtime_error);
        g = geometry();
        const auto result = run();
        QCOMPARE(result.report.at("unique_primitive_ordinals"), Json::array({0, 1}));
        // Range observations cannot silently become a unique original PS invocation.
        const auto code = compileHlsl("uint2 main(nointerpolation uint2 "
                                      "ordinal:FLORA_CANDIDATE_ORDINAL):SV_Target{return ordinal;}",
                                      "ps_5_0", "main", "native-pixel-candidate.hlsl")
                              .bytecode;
        QVERIFY_EXCEPTION_THROWN(pixelTraceInputs(code, {{0, result.observations}}), std::runtime_error);
        selection.layer = 1;
        const auto single = run();
        QCOMPARE(single.observations.records.size(), size_t(1));
        // Even one range observation must not bypass the invocation provenance gate.
        QVERIFY_EXCEPTION_THROWN(pixelTraceInputs(code, {{0, single.observations}}), std::runtime_error);
        validate(d.device.Get());
    }
};
QTEST_GUILESS_MAIN(NativePixelCandidatesTests)
#include "NativePixelCandidatesTests.moc"
