#include "application/DxbcCoverage.h"
#include "application/DxbcInspection.h"
#include "replay/Replay.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
#include <d3dcompiler.h>

using namespace flora;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> compile(const std::string &source, const char *profile = "ps_5_0") {
    Com<ID3DBlob> code, errors;
    const auto hr = D3DCompile(source.data(), source.size(), "coverage-test", nullptr, nullptr, "main",
                               profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                        : "Compile failed");
    const auto bytes = static_cast<const uint8_t *>(code->GetBufferPointer());
    return {bytes, bytes + code->GetBufferSize()};
}
uint32_t uintValue(const Json &v) {
    if ((!v.is_number_integer() && !v.is_number_unsigned()) ||
        (v.is_number_integer() && !v.is_number_unsigned() && v.get<int64_t>() < 0) ||
        v.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("Expected uint32");
    return v.get<uint32_t>();
}
Json process(const Json &job) {
    const auto base64 = QByteArray::fromStdString(job.at("input").get<std::string>());
    const auto input = QByteArray::fromBase64(base64, QByteArray::AbortOnBase64DecodingErrors);
    if (input.isEmpty())
        throw std::runtime_error("Invalid shader input");
    Bytes raw(reinterpret_cast<const uint8_t *>(input.data()), input.size());
    std::vector<uint8_t> output;
    Json result;
    const auto action = job.at("action").get<std::string>();
    if (action == "add")
        output = addCoverageMarker(raw, uintValue(job.at("slot")));
    else if (action == "replace") {
        CoverageMarkerOptions o;
        o.slot = uintValue(job.at("slot"));
        o.keepAlpha = job.value("keep_alpha", false);
        if (job.contains("array_index") && !job["array_index"].is_null())
            o.arrayIndex = uintValue(job["array_index"]);
        o.arrayRouted = job.value("array_routed", false);
        o.arraySource = job.value("array_source", Json(nullptr));
        output = replaceCoverageMarker(raw, o);
    } else if (action == "relocate") {
        std::map<uint32_t, uint32_t> mapping;
        for (const auto &pair : job.at("mapping"))
            mapping[uintValue(pair[0])] = uintValue(pair[1]);
        auto relocated = relocateShaderUavs(raw, mapping, uintValue(job.value("slot_count", Json(8))));
        output = std::move(relocated.bytes);
        result["declared"] = relocated.declared;
    } else if (action == "reserve") {
        std::set<uint32_t> bound;
        for (const auto &slot : job.at("bound"))
            bound.insert(uintValue(slot));
        auto reserved = reserveCoverageTarget(raw, bound, uintValue(job.value("slot_count", Json(8))));
        output = std::move(reserved.bytes);
        result["mapping"] = Json::object();
        for (const auto &[from, to] : reserved.mapping)
            result["mapping"][std::to_string(from)] = to;
    } else
        throw std::runtime_error("Unknown shader patch operation");
    result["output"] =
        QByteArray(reinterpret_cast<const char *>(output.data()), output.size()).toBase64().toStdString();
    return result;
}
std::vector<uint8_t> draw(Bytes shader, bool warp) {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
          "Create test device");
    const auto vertex = compile(
        "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}", "vs_5_0");
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    check(device->CreateVertexShader(vertex.data(), vertex.size(), nullptr, &vs), "Create test VS");
    check(device->CreatePixelShader(shader.data(), shader.size(), nullptr, &ps), "Create patched PS");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Com<ID3D11Texture2D> texture;
    Com<ID3D11RenderTargetView> view;
    check(device->CreateTexture2D(&desc, nullptr, &texture), "Create test texture");
    check(device->CreateRenderTargetView(texture.Get(), nullptr, &view), "Create test RTV");
    const float clear[4]{};
    context->ClearRenderTargetView(view.Get(), clear);
    auto pointer = view.Get();
    context->OMSetRenderTargets(1, &pointer, nullptr);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
    context->RSSetViewports(1, &viewport);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    Com<ID3D11RasterizerState> rs;
    check(device->CreateRasterizerState(&raster, &rs), "Create test rasterizer");
    context->RSSetState(rs.Get());
    context->Draw(3, 0);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&desc, nullptr, &staging), "Create test staging");
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Read marker pixels");
    std::vector<uint8_t> pixels(8 * 8 * 4);
    for (unsigned y = 0; y < 8; ++y)
        std::memcpy(pixels.data() + y * 32, static_cast<const uint8_t *>(map.pData) + y * map.RowPitch, 32);
    context->Unmap(staging.Get(), 0);
    return pixels;
}
} // namespace
class DxbcCoverageTests final : public QObject {
    Q_OBJECT
  private slots:
    void markersPreserveDiscardAndAlpha() {
        const auto raw = compile(
            "float4 main(float4 p:SV_Position):SV_Target{if(p.x<4)discard;return float4(.2,.3,.4,.5);}");
        CoverageMarkerOptions o;
        o.keepAlpha = true;
        const auto replaced = replaceCoverageMarker(raw, o);
        for (bool warp : {false, true}) {
            const auto original = draw(raw, warp), patched = draw(replaced, warp);
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x) {
                    const auto n = (y * 8 + x) * 4;
                    QCOMPARE(patched[n], uint8_t(x >= 4 ? 255 : 0));
                    QCOMPARE(patched[n + 1], patched[n]);
                    QCOMPARE(patched[n + 2], patched[n]);
                    QCOMPARE(patched[n + 3], original[n + 3]);
                }
        }
    }
    void targetlessIndexDoesNotDiscard() {
        const auto raw = compile("float4 main():SV_Target{return float4(.2,.3,.4,.5);}");
        for (uint32_t index : {0u, 7u, UINT32_MAX}) {
            CoverageMarkerOptions o;
            o.arrayIndex = index;
            const auto patched = replaceCoverageMarker(raw, o);
            for (bool warp : {false, true})
                for (auto value : draw(patched, warp))
                    QCOMPARE(value, uint8_t(index == 0 ? 255 : 0));
        }
    }
    void slotsAndReflections() {
        const auto raw = compile("RWBuffer<uint> v:register(u0);void main(float4 "
                                 "p:SV_Position){v[uint(p.x)]=0x0011e000u;}");
        const auto reserved = reserveCoverageTarget(raw, {0, 3});
        QCOMPARE(reserved.mapping, (std::map<uint32_t, uint32_t>{{0, 1}, {3, 4}}));
        QCOMPARE(relocateShaderUavs(reserved.bytes, {}).declared, std::set<uint32_t>{1});
        QCOMPARE(relocateShaderUavs(raw, {{0, 63}}, 64).declared, std::set<uint32_t>{0});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, reserveCoverageTarget(raw, {0, 1, 2, 3, 4, 5, 6, 7}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, relocateShaderUavs(raw, {{0, 8}}));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 addCoverageMarker(compile("float4 main():SV_Target{return 1;}"), 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, addCoverageMarker(raw, 8));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            relocateShaderUavs(compile("float4 main():SV_Target{return 1;}", "ps_4_0"), {}));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile file(args[2]);
        if (!file.open(QIODevice::ReadOnly))
            return 2;
        const auto jobs = Json::parse(file.readAll().toStdString());
        Json results = Json::array();
        for (const auto &job : jobs)
            try {
                results.push_back(process(job));
            } catch (const std::exception &e) {
                results.push_back({{"error", e.what()}});
            }
        QFile output(args[3]);
        if (!output.open(QIODevice::WriteOnly))
            return 3;
        output.write(QByteArray::fromStdString(results.dump()));
        return 0;
    }
    DxbcCoverageTests test;
    return QTest::qExec(&test, argc, argv);
}
#include "DxbcCoverageTests.moc"
