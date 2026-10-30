#include "application/DxbcPixelCompletion.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QtTest>
#include <d3d11sdklayers.h>
using namespace flora;
using Json = nlohmann::json;
namespace {
Com<ID3D11Device> makeDevice(uint32_t vendor) {
    Com<IDXGIAdapter1> selected;
    if (vendor) {
        Com<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create ordinal factory");
        for (UINT i = 0;; ++i) {
            Com<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Describe ordinal adapter");
            if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = adapter;
                break;
            }
        }
        if (!selected)
            throw std::runtime_error("Requested ordinal adapter is unavailable");
    }
    Com<ID3D11Device> result;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                            D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &result, nullptr,
                            nullptr),
          "Create ordinal device");
    return result;
}
void save(const QString &path, Bytes bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
            qint64(bytes.size()))
        throw std::runtime_error("Cannot save ordinal evidence");
}
void save(const QString &path, const Json &value) {
    const auto text = value.dump(2);
    save(path, Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
void wait(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Query *query) {
    context->Flush();
    QElapsedTimer elapsed;
    elapsed.start();
    while (context->GetData(query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) {
        check(device->GetDeviceRemovedReason(), "Ordinal device was removed");
        if (elapsed.elapsed() > 5000)
            throw std::runtime_error("Ordinal GPU completion timed out");
        QThread::msleep(1);
    }
    check(context->GetData(query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH), "Read ordinal completion");
}
} // namespace
class NativePixelOrdinalTests final : public QObject {
    Q_OBJECT
  private slots:
    void extraction_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("layout");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<uint32_t>("targets");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t layout = 0; layout != 3; ++layout)
                for (uint32_t samples : {1u, 4u})
                    for (uint32_t mode = 0; mode != 4; ++mode)
                        for (uint32_t targets : {1u, 8u})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(layout)
                                                         .arg(samples)
                                                         .arg(mode)
                                                         .arg(targets)))
                                << vendor << layout << samples << mode << targets;
    }
    void extraction() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, layout);
        QFETCH(uint32_t, samples);
        QFETCH(uint32_t, mode);
        QFETCH(uint32_t, targets);
        const UINT extent = qEnvironmentVariableIsSet("FLORA_PIXEL_ORDINAL_EXTENT")
                                ? qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_EXTENT")
                                : 4;
        const bool floatOutput = qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_FLOAT") != 0;
        const UINT stencilMip = qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_STENCIL_MIP");
        const bool wideStencil = qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_WIDE_STENCIL") != 0;
        const UINT coverageDiscard = qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_COVERAGE_DISCARD");
        const bool rasterMultisample = !qEnvironmentVariableIsSet("FLORA_PIXEL_ORDINAL_RASTER_MSAA") ||
                                       qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_RASTER_MSAA") != 0;
        QVERIFY(extent == 1 || extent == 4);
        QVERIFY(stencilMip == 0 || (stencilMip == 2 && samples == 1));
        QVERIFY(coverageDiscard <= 2);
        auto device = makeDevice(vendor);
        Com<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        Com<IDXGIDevice> dxgi;
        check(device.As(&dxgi), "Get ordinal DXGI device");
        Com<IDXGIAdapter> adapter;
        check(dxgi->GetAdapter(&adapter), "Get ordinal actual adapter");
        DXGI_ADAPTER_DESC adapterDesc{};
        check(adapter->GetDesc(&adapterDesc), "Get ordinal actual identity");
        if (vendor)
            QCOMPARE(adapterDesc.VendorId, vendor);
        const std::string structure =
            "struct V {float4 p:SV_Position; nointerpolation uint4 identity:TEXCOORD0;};";
        const auto vsCode =
            compileHlsl(structure +
                            "V main(uint v:SV_VertexID,uint i:SV_InstanceID){V o;"
                            "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};"
                            "o.p=float4(p[v%3],0.5,1);o.identity=uint4(" +
                            (layout == 1 ? std::string("i") : std::string("v/3")) + ",i,0,0);return o;}",
                        "vs_5_0")
                .bytecode;
        const auto gsCode =
            compileHlsl(structure +
                            "struct G {float4 p:SV_Position;nointerpolation uint4 identity:TEXCOORD0;"
                            "uint primitive:SV_PrimitiveID;};"
                            "[maxvertexcount(6)] void main(triangle V v[3],inout TriangleStream<G> s){"
                            "for(uint j=0;j<2;++j){for(uint k=0;k<3;++k){G o; o.p=v[k].p;"
                            "o.identity=v[k].identity;o.identity.x=2*v[k].identity.x+j;"
                            "o.primitive=7;s.Append(o);}s."
                            "RestartStrip();}}",
                        "gs_5_0")
                .bytecode;
        std::string pixel = structure + "struct O {";
        for (UINT i = 0; i != targets; ++i)
            pixel += std::string(floatOutput ? "float4 c" : "uint4 c") + std::to_string(i) + ":SV_Target" +
                     std::to_string(i) + ";";
        if (mode == 3 && coverageDiscard != 1)
            pixel += "uint coverage:SV_Coverage;";
        pixel += "};";
        if (mode == 2)
            pixel += "[earlydepthstencil]";
        pixel += "O main(V v,uint p:SV_PrimitiveID,uint s:SV_SampleIndex){O o;";
        if (mode == 1 || mode == 2)
            pixel += "if(v.identity.x&1)discard;";
        if (mode == 3 && coverageDiscard != 1)
            pixel += coverageDiscard ? "o.coverage=0xffffffff;"
                                     : "o.coverage=((v.identity.x&1)==(s&1))?0xffffffff:0;";
        for (UINT i = 0; i != targets; ++i)
            pixel += "o.c" + std::to_string(i) + "=uint4(v.identity.x+1,p,v.identity.y,s);";
        if (mode == 3 && coverageDiscard)
            pixel += "if((v.identity.x&1)!=(s&1))discard;";
        pixel += "return o;}";
        const auto sourcePsCode = compileHlsl(pixel, "ps_5_0").bytecode;
        auto psCode = sourcePsCode;
        Json normalization = nullptr;
        if (qEnvironmentVariableIntValue("FLORA_PIXEL_ORDINAL_NORMALIZE_COVERAGE")) {
            QVERIFY(samples == 1);
            PixelCompletionShader normalized;
            try {
                normalized = normalizeSingleSamplePixelCoverage(sourcePsCode);
            } catch (const std::exception &error) {
                QFAIL(error.what());
            }
            psCode = std::move(normalized.bytecode);
            normalization = std::move(normalized.identity);
        }
        const std::string type = floatOutput ? "float4" : "uint4";
        const auto coordinate = std::to_string(extent == 1 ? 0 : 1);
        const auto load = samples == 1 ? "t.Load(int3(" + coordinate + "," + coordinate + ",0))"
                                       : "t.Load(int2(" + coordinate + "," + coordinate + "),p.x)";
        const auto csCode =
            compileHlsl((samples == 1 ? "Texture2D<" + type + ">" : "Texture2DMS<" + type + ",4>") +
                            " t:register(t0);RWStructuredBuffer<uint4> o:register(u0);"
                            "[numthreads(1,1,1)]void main(uint3 p:SV_DispatchThreadID){o[p.x]=" +
                            (floatOutput ? "asuint(" + load + ")" : load) + ";}",
                        "cs_5_0")
                .bytecode;
        Com<ID3D11VertexShader> vs;
        Com<ID3D11GeometryShader> gs;
        Com<ID3D11PixelShader> ps;
        Com<ID3D11ComputeShader> cs;
        check(device->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vs), "Create ordinal VS");
        check(device->CreateGeometryShader(gsCode.data(), gsCode.size(), nullptr, &gs), "Create ordinal GS");
        check(device->CreatePixelShader(psCode.data(), psCode.size(), nullptr, &ps), "Create ordinal PS");
        check(device->CreateComputeShader(csCode.data(), csCode.size(), nullptr, &cs),
              "Create ordinal reader");
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = extent;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = floatOutput ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32A32_UINT;
        texture.SampleDesc.Count = samples;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        std::vector<Com<ID3D11RenderTargetView>> views(targets);
        std::vector<ID3D11RenderTargetView *> bindings;
        Com<ID3D11ShaderResourceView> source;
        for (UINT i = 0; i != targets; ++i) {
            Com<ID3D11Texture2D> color;
            check(device->CreateTexture2D(&texture, nullptr, &color), "Create ordinal color");
            check(device->CreateRenderTargetView(color.Get(), nullptr, &views[i]), "Create ordinal RTV");
            bindings.push_back(views[i].Get());
            if (i == targets - 1)
                check(device->CreateShaderResourceView(color.Get(), nullptr, &source), "Create ordinal SRV");
        }
        texture.Format = wideStencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
        texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        texture.Width <<= stencilMip;
        texture.Height <<= stencilMip;
        texture.MipLevels += stencilMip;
        Com<ID3D11Texture2D> depth;
        Com<ID3D11DepthStencilView> dsv;
        check(device->CreateTexture2D(&texture, nullptr, &depth), "Create ordinal stencil");
        D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};
        depthView.Format = texture.Format;
        depthView.ViewDimension =
            samples == 1 ? D3D11_DSV_DIMENSION_TEXTURE2D : D3D11_DSV_DIMENSION_TEXTURE2DMS;
        depthView.Texture2D.MipSlice = stencilMip;
        check(device->CreateDepthStencilView(depth.Get(), &depthView, &dsv), "Create ordinal DSV");
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = ds.StencilWriteMask = 0xff;
        ds.FrontFace = {D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_INCR_SAT, D3D11_STENCIL_OP_INCR_SAT,
                        D3D11_COMPARISON_EQUAL};
        ds.BackFace = ds.FrontFace;
        Com<ID3D11DepthStencilState> ordinalState;
        Com<ID3D11DepthStencilState> referenceState;
        check(device->CreateDepthStencilState(&ds, &ordinalState), "Create ordinal selector");
        ds.StencilEnable = FALSE;
        check(device->CreateDepthStencilState(&ds, &referenceState), "Create ordinal reference state");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.MultisampleEnable = rasterMultisample;
        Com<ID3D11RasterizerState> rasterizer;
        check(device->CreateRasterizerState(&raster, &rasterizer), "Create ordinal rasterizer");
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 16 * samples;
        buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        buffer.StructureByteStride = 16;
        Com<ID3D11Buffer> output;
        Com<ID3D11Buffer> staging;
        Com<ID3D11UnorderedAccessView> uav;
        check(device->CreateBuffer(&buffer, nullptr, &output), "Create ordinal readback output");
        check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create ordinal readback UAV");
        buffer.BindFlags = buffer.MiscFlags = buffer.StructureByteStride = 0;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        buffer.Usage = D3D11_USAGE_STAGING;
        check(device->CreateBuffer(&buffer, nullptr, &staging), "Create ordinal staging");
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        Com<ID3D11Query> complete;
        check(device->CreateQuery(&queryDesc, &complete), "Create ordinal completion");
        queryDesc.Query = D3D11_QUERY_OCCLUSION;
        Com<ID3D11Query> occlusion;
        check(device->CreateQuery(&queryDesc, &occlusion), "Create ordinal occlusion");
        Json report{{"stencil_mip", stencilMip},
                    {"coverage_normalization", normalization},
                    {"stencil_format", uint32_t(texture.Format)},
                    {"coverage_discard", coverageDiscard},
                    {"extent", extent},
                    {"float_output", floatOutput},
                    {"raster_multisample", rasterMultisample},
                    {"schema", 1},
                    {"requested_vendor", vendor},
                    {"actual_vendor", adapterDesc.VendorId},
                    {"actual_device", adapterDesc.DeviceId},
                    {"adapter_luid", {adapterDesc.AdapterLuid.HighPart, adapterDesc.AdapterLuid.LowPart}},
                    {"layout", layout},
                    {"samples", samples},
                    {"mode", mode},
                    {"targets", targets},
                    {"scope", "owned_draw_stencil_ordinal_probe"},
                    {"original_fragment_eligibility", nullptr},
                    {"rows", Json::array()}};
        for (UINT count : {8u, 260u}) {
            for (int rank : {-1, 0, 1, 2, 6, 7, 8, 127, 253, 254, 255}) {
                context->ClearState();
                for (auto *view : bindings) {
                    const float clear[4]{};
                    context->ClearRenderTargetView(view, clear);
                }
                context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
                context->OMSetRenderTargets(targets, bindings.data(), dsv.Get());
                context->OMSetDepthStencilState(rank < 0 ? referenceState.Get() : ordinalState.Get(),
                                                UINT(rank));
                context->RSSetState(rasterizer.Get());
                const D3D11_VIEWPORT viewport{0, 0, float(extent), float(extent), 0, 1};
                context->RSSetViewports(1, &viewport);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vs.Get(), nullptr, 0);
                context->GSSetShader(layout == 2 ? gs.Get() : nullptr, nullptr, 0);
                context->PSSetShader(ps.Get(), nullptr, 0);
                context->Begin(occlusion.Get());
                if (layout == 1)
                    context->DrawInstanced(3, count, 0, 0);
                else
                    context->Draw(3 * (layout == 2 ? count / 2 : count), 0);
                context->End(occlusion.Get());
                context->OMSetRenderTargets(0, nullptr, nullptr);
                context->CSSetShader(cs.Get(), nullptr, 0);
                auto *read = source.Get();
                context->CSSetShaderResources(0, 1, &read);
                auto *write = uav.Get();
                context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
                context->Dispatch(samples, 1, 1);
                context->CopyResource(staging.Get(), output.Get());
                context->End(complete.Get());
                wait(device.Get(), context.Get(), complete.Get());
                UINT64 passing = 0;
                check(context->GetData(occlusion.Get(), &passing, sizeof passing, 0),
                      "Read ordinal occlusion");
                D3D11_MAPPED_SUBRESOURCE mapped{};
                check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map ordinal outputs");
                std::vector<std::array<uint32_t, 4>> actual(samples);
                std::memcpy(actual.data(), mapped.pData, 16 * samples);
                context->Unmap(staging.Get(), 0);
                std::vector<std::array<uint32_t, 4>> expected(samples);
                for (UINT sample = 0; sample != samples; ++sample) {
                    UINT stencil = 0;
                    for (UINT fragment = 0; fragment != count; ++fragment) {
                        const bool discard = (mode == 1 || mode == 2) && (fragment & 1);
                        const bool masked = mode == 3 && ((fragment & 1) != (sample & 1));
                        const bool matches = rank < 0 || stencil == UINT(rank);
                        if (mode == 2 || (!discard && !masked))
                            stencil = std::min(stencil + 1, 255u);
                        if (!matches || discard || masked)
                            continue;
                        expected[sample] = {fragment + 1,
                                            layout == 2   ? 7u
                                            : layout == 1 ? 0u
                                                          : fragment,
                                            layout == 1 ? fragment : 0u, sample};
                    }
                }
                if (floatOutput)
                    for (auto &value : expected)
                        for (auto &component : value) {
                            const float converted = float(component);
                            std::memcpy(&component, &converted, 4);
                        }
                report["rows"].push_back({{"count", count},
                                          {"rank", rank},
                                          {"outputs", actual},
                                          {"expected", expected},
                                          {"occlusion", passing}});
            }
        }
        context->ClearState();
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Get ordinal validation queue");
        report["diagnostics"] = Json::array();
        for (UINT64 i = 0; i != queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Get ordinal diagnostic size");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size), "Get ordinal diagnostic");
            report["diagnostics"].push_back(
                {{"severity", uint32_t(message->Severity)}, {"text", message->pDescription}});
        }
        const auto root = qEnvironmentVariable("FLORA_PIXEL_ORDINAL_EVIDENCE");
        if (!root.isEmpty()) {
            const auto directory = root + "/" + QString::fromLatin1(QTest::currentDataTag());
            if (!QDir().mkpath(directory))
                throw std::runtime_error("Cannot create ordinal evidence directory");
            save(directory + "/results.json", report);
            save(directory + "/vs.dxbc", Bytes(vsCode));
            save(directory + "/gs.dxbc", Bytes(gsCode));
            save(directory + "/ps.dxbc", Bytes(psCode));
            if (!normalization.is_null())
                save(directory + "/source.ps.dxbc", Bytes(sourcePsCode));
            save(directory + "/readback.dxbc", Bytes(csCode));
        }
        QVERIFY2(report["diagnostics"].empty(), report["diagnostics"].dump().c_str());
        for (const auto &row : report["rows"])
            QVERIFY2(row["outputs"] == row["expected"], row.dump().c_str());
        if (mode == 0) {
            for (const auto &row : report["rows"]) {
                const auto count = row["count"].get<UINT>();
                const auto rank = row["rank"].get<int>();
                const auto selected = rank < 0      ? count
                                      : rank == 255 ? (count > 255 ? count - 255 : 0)
                                                    : UINT(rank < int(count));
                QVERIFY2(row["occlusion"] == uint64_t(extent) * extent * samples * selected,
                         row.dump().c_str());
            }
        }
    }
};
QTEST_GUILESS_MAIN(NativePixelOrdinalTests)
#include "NativePixelOrdinalTests.moc"
