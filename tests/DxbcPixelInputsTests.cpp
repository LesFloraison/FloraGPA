#include "application/DxbcGpuCompletion.h"
#include "application/DxbcPixelGeometry.h"
#include "application/DxbcPixelInputs.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
#include <QtTest>
#include <bit>
#include <d3d11sdklayers.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <psapi.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::array<uint32_t, 4>;
// Independent oracle: one bit at a time, first gather a column within each
// row, then gather the target row. It never uses mixed derivatives or bytes.
const char *quadOracle = R"(
uint4 gather(uint4 v,float2 role,uint lane) {
    uint4 result=0;
    [loop] for(uint bit=0;bit<32;++bit) {
        float4 b=float4((v>>bit)&1u);
        float4 horizontal=b+(float(lane&1u)-role.x)*ddx_fine(b);
        float4 value=horizontal+(float(lane>>1u)-role.y)*ddy_fine(horizontal);
        result|=uint4(value)<<bit;
    }
    return result;
}
struct Q {uint4 a:SV_Target0;uint4 b:SV_Target1;uint4 c:SV_Target2;uint4 d:SV_Target3;uint4 sample:SV_Target4;};
)";
std::vector<uint8_t> compile(const std::string &source, const std::string &profile = "ps_5_0") {
    return compileHlsl(source, profile).bytecode;
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11InfoQueue> messages;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create pixel input factory");
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> adapter;
                const auto hr = factory->EnumAdapters1(i, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate pixel input adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Describe pixel input adapter");
                if (desc.VendorId == vendor) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested pixel input adapter unavailable");
        }
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create pixel input device");
        check(device.As(&messages), "Query pixel input validation");
    }
    void validate() {
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(messages->GetMessage(i, nullptr, &size), "Size pixel input validation");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(messages->GetMessage(i, message, &size), "Read pixel input validation");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        messages->ClearStoredMessages();
    }
};
uint32_t inputRegister(Bytes code, const char *semantic) {
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "Reflect pixel input");
    D3D11_SHADER_DESC desc{};
    check(reflection->GetDesc(&desc), "Describe pixel input");
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC field{};
        check(reflection->GetInputParameterDesc(i, &field), "Read pixel input field");
        if (std::string(field.SemanticName) == semantic)
            return field.Register;
    }
    throw std::runtime_error("Missing pixel input semantic");
}
struct Target {
    Com<ID3D11Texture2D> texture;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11ShaderResourceView> srv;
};
PostTransformGeometry captureGeometry(Device &d, Bytes vs) {
    auto layout = postTransformLayout(vs, 0);
    const auto stride = layout.at("stride").get<uint32_t>();
    std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
    for (const auto &a : layout.at("attributes"))
        entries.push_back({0, a.at("semantic").get_ref<const std::string &>().c_str(),
                           a.at("index").get<UINT>(), BYTE(a.at("components").at(0).get<unsigned>()),
                           BYTE(a.at("component_count").get<unsigned>()), 0});
    Com<ID3D11GeometryShader> so;
    check(d.device->CreateGeometryShaderWithStreamOutput(vs.data(), vs.size(), entries.data(),
                                                         UINT(entries.size()), &stride, 1,
                                                         D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &so),
          "Create geometry fixture SO shader");
    Com<ID3D11Buffer> output, staging;
    D3D11_BUFFER_DESC bd{stride * 3, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
    check(d.device->CreateBuffer(&bd, nullptr, &output), "Create geometry fixture SO buffer");
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(d.device->CreateBuffer(&bd, nullptr, &staging), "Create geometry fixture staging");
    d.context->GSSetShader(so.Get(), nullptr, 0);
    UINT offset = 0;
    d.context->SOSetTargets(1, output.GetAddressOf(), &offset);
    d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    d.context->Draw(3, 0);
    d.context->SOSetTargets(0, nullptr, nullptr);
    d.context->GSSetShader(nullptr, nullptr, 0);
    d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.context->CopyResource(staging.Get(), output.Get());
    GpuCompletion completion(d.device.Get(), d.context.Get());
    completion.submit();
    const auto mapped = completion.map(staging.Get());
    PostTransformGeometry result;
    result.bytes.resize(stride * 3);
    std::memcpy(result.bytes.data(), mapped.pData, result.bytes.size());
    d.context->Unmap(staging.Get(), 0);
    result.report = std::move(layout);
    result.report.update({{"geometry_stage", "final_pre_rasterization"},
                          {"vertices", 3},
                          {"primitives", 1},
                          {"vertices_per_primitive", 3},
                          {"statistics_only", false}});
    d.validate();
    return result;
}
std::vector<Words> read(Device &d, Target &target, unsigned samples) {
    const auto declaration =
        samples == 1 ? "Texture2D<uint4> input:register(t0);" : "Texture2DMS<uint4> input:register(t0);";
    const auto expression = samples == 1 ? "input.Load(int3(id.xy,0))" : "input.Load(id.xy,s)";
    const auto source = std::string(declaration) +
                        "RWStructuredBuffer<uint4> output:register(u0);[numthreads(8,8,1)]"
                        "void main(uint3 id:SV_DispatchThreadID){for(uint s=0;s<" +
                        std::to_string(samples) + ";++s)output[(id.y*16+id.x)*" + std::to_string(samples) +
                        "+s]=" + expression + ";}";
    auto code = compile(source, "cs_5_0");
    Com<ID3D11ComputeShader> cs;
    check(d.device->CreateComputeShader(code.data(), code.size(), nullptr, &cs), "Create readback shader");
    D3D11_BUFFER_DESC bd{
        UINT(16 * 16 * samples * sizeof(Words)), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,   sizeof(Words)};
    Com<ID3D11Buffer> output, staging;
    check(d.device->CreateBuffer(&bd, nullptr, &output), "Create readback output");
    Com<ID3D11UnorderedAccessView> uav;
    check(d.device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create readback UAV");
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(d.device->CreateBuffer(&bd, nullptr, &staging), "Create readback staging");
    d.context->CSSetShader(cs.Get(), nullptr, 0);
    d.context->CSSetShaderResources(0, 1, target.srv.GetAddressOf());
    d.context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    d.context->Dispatch(2, 2, 1);
    ID3D11ShaderResourceView *noSrv = nullptr;
    ID3D11UnorderedAccessView *noUav = nullptr;
    d.context->CSSetShaderResources(0, 1, &noSrv);
    d.context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    d.context->CopyResource(staging.Get(), output.Get());
    GpuCompletion completion(d.device.Get(), d.context.Get(), {}, 5000);
    completion.submit();
    const auto mapped = completion.map(staging.Get());
    std::vector<Words> result(16 * 16 * samples);
    std::memcpy(result.data(), mapped.pData, bd.ByteWidth);
    d.context->Unmap(staging.Get(), 0);
    return result;
}
} // namespace
class DxbcPixelInputsTests : public QObject {
    Q_OBJECT
  private slots:
    void capture_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint32_t>("samples");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode :
                 {"linear", "noperspective", "centroid", "noperspective centroid", "sample",
                  "noperspective sample", "nointerpolation", "uint", "sint", "sparse", "min16", "trigger",
                  "position", "position-z", "sample-id", "sm4", "sm41"})
                for (uint32_t samples : {1u, 4u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(mode).arg(samples)))
                        << vendor << QString(mode) << samples;
    }
    void capture() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, mode);
        QFETCH(uint32_t, samples);
        try {
            Device d(vendor);
            const bool integer = mode == "uint" || mode == "sint";
            const bool system = mode.startsWith("position") || mode == "sample-id";
            const bool sparse = mode == "sparse" || mode == "position-z";
            std::string qualifier = "linear", type = "float4";
            if (integer) {
                qualifier = "nointerpolation";
                type = mode == "uint" ? "uint4" : "int4";
            } else if (mode == "min16")
                type = "min16float4";
            else if (!system && !sparse && mode != "trigger" && mode != "sm4" && mode != "sm41")
                qualifier = mode.toStdString();
            const std::string value = integer
                                          ? (mode == "uint" ? "uint4(0x80000000,0x7fc12345,0xffffffff,1)"
                                                            : "int4(-123,-2147483647,12345,-1)")
                                          : "float4(id*3.125+0.125,id*0.5-0.25,id*1.25+0.375,id*4.5-2.75)";
            const std::string vsBody = "O main(uint id:SV_VertexID){O o;float2 "
                                       "p=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);"
                                       "float w=id==0?1:id==1?2:4;o.p=float4(p*w,0.25*w,w);o.v=" +
                                       value + ";return o;}";
            const auto vs =
                compile("struct O{float4 p:SV_Position;" + type + " v:DATA;};" + vsBody, "vs_5_0");
            std::string psSource;
            if (mode.startsWith("position"))
                psSource = "uint4 main(float4 p:SV_Position):SV_Target{return asuint(" +
                           std::string(mode == "position-z" ? "float4(0,0,p.z,0)" : "p") + ");}";
            else if (mode == "sample-id")
                psSource = "uint4 main(float4 p:SV_Position,uint s:SV_SampleIndex):SV_Target{return "
                           "uint4(s,0,0,0);}";
            else
                psSource =
                    "struct O{uint4 value:SV_Target0;uint4 sample:SV_Target1;};O main(float4 p:SV_Position," +
                    qualifier + " " + type + " v:DATA" + (mode == "trigger" ? ",uint s:SV_SampleIndex" : "") +
                    "){O o;o.value=asuint(" +
                    (sparse            ? std::string("float4(v.x,0,v.z,0)")
                     : mode == "min16" ? "float4(v)"
                                       : "v") +
                    ");o.sample=" + (mode == "trigger" ? "s" : "0") + ";return o;}";
            auto ps = compile(psSource, mode == "sm4" ? "ps_4_0" : mode == "sm41" ? "ps_4_1" : "ps_5_0");
            const auto selected = inputRegister(ps, mode.startsWith("position") ? "SV_Position"
                                                    : mode == "sample-id"       ? "SV_SampleIndex"
                                                                                : "DATA");
            const auto helper = pixelInputShader(ps, selected);
            QCOMPARE(helper.mask, mode == "position-z" ? 4u : mode == "sample-id" ? 1u : sparse ? 5u : 15u);
            QCOMPARE(helper.sampleFrequency, mode == "sample" || mode == "noperspective sample" ||
                                                 mode == "trigger" || mode == "sample-id");
            Com<ID3D11VertexShader> vertex, compactVertex;
            const std::string parameters =
                "float4 p:SV_Position" + (system ? std::string() : "," + qualifier + " " + type + " v:DATA") +
                (helper.sampleFrequency ? ",uint s:SV_SampleIndex" : "");
            const std::string raw = mode == "sample-id"    ? "uint4(s,0,0,0)"
                                    : mode == "position"   ? "asuint(p)"
                                    : mode == "position-z" ? "asuint(float4(0,0,p.z,0))"
                                    : sparse               ? "asuint(float4(v.x,0,v.z,0))"
                                    : mode == "min16"      ? "asuint(float4(v))"
                                                           : "asuint(v)";
            const auto oracleSource =
                std::string(quadOracle) + "Q main(" + parameters +
                "){float2 parity=float2(int2(floor(p.xy))&1);"
                "float2 direction=float2(ddx_fine(parity.x),ddy_fine(parity.y))*(1-2*parity);"
                "float2 role=float2(direction.x<0,direction.y<0);uint4 rawBits=" +
                raw +
                ";Q "
                "o;o.a=gather(rawBits,role,0);o.b=gather(rawBits,role,1);o.c=gather(rawBits,role,2);o.d="
                "gather(rawBits,role,3);o.sample=" +
                (helper.sampleFrequency ? "s" : "0") + ";return o;}";
            const auto oracleCode = compile(oracleSource);
            Com<ID3D11PixelShader> reference, capture, oracle;
            check(d.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex),
                  "Create input fixture VS");
            check(d.device->CreatePixelShader(ps.data(), ps.size(), nullptr, &reference),
                  "Create input reference PS");
            check(d.device->CreatePixelShader(oracleCode.data(), oracleCode.size(), nullptr, &oracle),
                  "Create independent quad oracle PS");
            check(d.device->CreatePixelShader(helper.bytecode.data(), helper.bytecode.size(), nullptr,
                                              &capture),
                  "Create input transport PS");
            std::array<Target, 8> targets;
            for (auto &target : targets) {
                D3D11_TEXTURE2D_DESC td{16,
                                        16,
                                        1,
                                        1,
                                        DXGI_FORMAT_R32G32B32A32_UINT,
                                        {samples, 0},
                                        D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                                        0,
                                        0};
                check(d.device->CreateTexture2D(&td, nullptr, &target.texture), "Create pixel input target");
                check(d.device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv),
                      "Create pixel input RTV");
                check(d.device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv),
                      "Create pixel input SRV");
            }
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = rd.MultisampleEnable = true;
            Com<ID3D11RasterizerState> raster;
            check(d.device->CreateRasterizerState(&rd, &raster), "Create pixel input rasterizer");
            d.context->RSSetState(raster.Get());
            const D3D11_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
            d.context->RSSetViewports(1, &viewport);
            d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            d.context->VSSetShader(vertex.Get(), nullptr, 0);
            const auto geometry = captureGeometry(d, vs);
            const auto bridge = pixelGeometryBridge(geometry, helper, 0);
            const auto &compactVs = bridge.vertexShader;
            Com<ID3D11GeometryShader> bridgeGs;
            Com<ID3D11InputLayout> bridgeLayout;
            Com<ID3D11Buffer> bridgeBuffer;
            check(d.device->CreateVertexShader(compactVs.data(), compactVs.size(), nullptr, &compactVertex),
                  "Create native compact geometry VS");
            check(d.device->CreateGeometryShader(bridge.geometryShader.data(), bridge.geometryShader.size(),
                                                 nullptr, &bridgeGs),
                  "Create native compact geometry GS");
            std::vector<D3D11_INPUT_ELEMENT_DESC> bridgeElements;
            for (UINT slot = 0; slot < bridge.stride / 16; ++slot)
                bridgeElements.push_back({"FLORA_GEOMETRY", slot, DXGI_FORMAT_R32G32B32A32_UINT, 0, slot * 16,
                                          D3D11_INPUT_PER_VERTEX_DATA, 0});
            check(d.device->CreateInputLayout(bridgeElements.data(), UINT(bridgeElements.size()),
                                              compactVs.data(), compactVs.size(), &bridgeLayout),
                  "Create native compact geometry layout");
            const D3D11_BUFFER_DESC bridgeDesc{
                UINT(bridge.vertices.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
            const D3D11_SUBRESOURCE_DATA bridgeData{bridge.vertices.data(), 0, 0};
            check(d.device->CreateBuffer(&bridgeDesc, &bridgeData, &bridgeBuffer),
                  "Create native compact geometry buffer");
            auto draw = [&](ID3D11PixelShader *shader, unsigned count) {
                std::array<ID3D11RenderTargetView *, 8> views{};
                const float clear[]{16777216, 16777216, 16777216, 16777216};
                for (unsigned i = 0; i < count; ++i) {
                    views[i] = targets[i].rtv.Get();
                    d.context->ClearRenderTargetView(views[i], clear);
                }
                d.context->OMSetRenderTargets(count, views.data(), nullptr);
                d.context->PSSetShader(shader, nullptr, 0);
                d.context->Draw(3, 0);
                d.context->OMSetRenderTargets(0, nullptr, nullptr);
            };
            draw(reference.Get(), system ? 1 : 2);
            const auto expected = read(d, targets[0], samples);
            d.validate();
            draw(oracle.Get(), 5);
            std::array<std::vector<Words>, 4> neighbors;
            for (unsigned i = 0; i < 4; ++i)
                neighbors[i] = read(d, targets[i], samples);
            d.validate();
            d.context->VSSetShader(compactVertex.Get(), nullptr, 0);
            d.context->GSSetShader(bridgeGs.Get(), nullptr, 0);
            d.context->IASetInputLayout(bridgeLayout.Get());
            const UINT bridgeOffset = 0;
            d.context->IASetVertexBuffers(0, 1, bridgeBuffer.GetAddressOf(), &bridge.stride, &bridgeOffset);
            draw(capture.Get(), 8);
            std::array<std::vector<Words>, 8> observed;
            for (unsigned i = 0; i < 8; ++i)
                observed[i] = read(d, targets[i], samples);
            const auto evidence = qEnvironmentVariable("FLORA_PIXEL_INPUT_EVIDENCE");
            if (!evidence.isEmpty()) {
                const auto name = QString("%1-%2-%3").arg(vendor).arg(mode).arg(samples).replace(' ', '-');
                const auto path = QDir(evidence).filePath(name);
                if (QDir(path).exists() || !QDir().mkpath(path))
                    throw std::runtime_error("Pixel input evidence directory must be new");
                auto save = [&](const QString &file, const void *data, size_t size) {
                    QFile out(QDir(path).filePath(file));
                    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                        out.write(static_cast<const char *>(data), qint64(size)) != qint64(size))
                        throw std::runtime_error("Cannot save pixel input evidence");
                };
                auto shader = [&](const QString &file, const std::vector<uint8_t> &bytes) {
                    save(file + ".dxbc", bytes.data(), bytes.size());
                    Com<ID3DBlob> assembly;
                    check(D3DDisassemble(bytes.data(), bytes.size(), 0, nullptr, &assembly),
                          "Disassemble pixel input evidence");
                    save(file + ".asm.txt", assembly->GetBufferPointer(), assembly->GetBufferSize());
                };
                shader("original-vs", vs);
                shader("compact-vs", compactVs);
                shader("bridge-gs", bridge.geometryShader);
                save("source-geometry.bin", geometry.bytes.data(), geometry.bytes.size());
                save("bridge-geometry.bin", bridge.vertices.data(), bridge.vertices.size());
                const auto sourceGeometry = geometry.report.dump(2), bridgeIdentity = bridge.identity.dump(2);
                save("source-geometry.json", sourceGeometry.data(), sourceGeometry.size());
                save("bridge-geometry.json", bridgeIdentity.data(), bridgeIdentity.size());
                shader("original-ps", ps);
                shader("helper-ps", helper.bytecode);
                shader("oracle-ps", oracleCode);
                save("original-ps.hlsl", psSource.data(), psSource.size());
                save("oracle-ps.hlsl", oracleSource.data(), oracleSource.size());
                save("reference.bin", expected.data(), expected.size() * sizeof(Words));
                for (unsigned i = 0; i < 4; ++i)
                    save(QString("oracle-%1.bin").arg(i), neighbors[i].data(),
                         neighbors[i].size() * sizeof(Words));
                for (unsigned i = 0; i < 8; ++i)
                    save(QString("target-%1.bin").arg(i), observed[i].data(),
                         observed[i].size() * sizeof(Words));
                Com<IDXGIDevice> dxgi;
                Com<IDXGIAdapter> adapter;
                check(d.device.As(&dxgi), "Query evidence DXGI device");
                check(dxgi->GetAdapter(&adapter), "Query evidence adapter");
                DXGI_ADAPTER_DESC desc{};
                LARGE_INTEGER driver{};
                check(adapter->GetDesc(&desc), "Describe evidence adapter");
                check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "Read evidence driver");
                nlohmann::json report{{"schema", 1},
                                      {"mode", mode.toStdString()},
                                      {"vendor_request", vendor},
                                      {"adapter_vendor", desc.VendorId},
                                      {"adapter_device", desc.DeviceId},
                                      {"adapter_luid", (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) |
                                                           desc.AdapterLuid.LowPart},
                                      {"driver_version", driver.QuadPart},
                                      {"samples", samples},
                                      {"mask", helper.mask},
                                      {"register", selected},
                                      {"sample_frequency", helper.sampleFrequency},
                                      {"modules", nlohmann::json::array()}};
                std::array<HMODULE, 1024> modules{};
                DWORD needed = 0;
                if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof modules, &needed) ||
                    needed > sizeof modules)
                    throw std::runtime_error("Cannot enumerate evidence modules");
                for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
                    std::array<wchar_t, 32768> module{};
                    const auto length = GetModuleFileNameW(modules[i], module.data(), DWORD(module.size()));
                    if (!length || length >= module.size())
                        throw std::runtime_error("Cannot read evidence module path");
                    report["modules"].push_back(
                        QString::fromWCharArray(module.data(), int(length)).toStdString());
                }
                const auto text = report.dump(2);
                save("report.json", text.data(), text.size());
            }
            size_t comparisons = 0;
            for (unsigned y = 2; y < 14; ++y)
                for (unsigned x = 2; x < 14; ++x)
                    for (unsigned s = 0; s < samples; ++s) {
                        const auto index = (y * 16 + x) * samples + s;
                        const auto location = observed[4][index];
                        QCOMPARE(location[0], x);
                        QCOMPARE(location[1], y);
                        QVERIFY(location[2] <= 1 && location[3] <= 1);
                        const auto role = location[2] + 2 * location[3];
                        QCOMPARE(helper.decode(observed[role][index]).valid, uint8_t(helper.mask));
                        for (unsigned c = 0; c < 4; ++c)
                            if (helper.mask & (1u << c))
                                QCOMPARE(observed[role][index][c], expected[index][c]);
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            const auto nx = x - location[2] + (lane & 1), ny = y - location[3] + (lane >> 1);
                            const auto &wanted = neighbors[lane][index];
                            for (unsigned c = 0; c < 4; ++c)
                                if (helper.mask & (1u << c)) {
                                    if (observed[lane][index][c] != wanted[c]) {
                                        qWarning() << "Pixel/lane/component" << x << y << s << lane << c
                                                   << "role" << role << "neighbor" << nx << ny;
                                    }
                                    QCOMPARE(observed[lane][index][c], wanted[c]);
                                    ++comparisons;
                                }
                            QCOMPARE(observed[5][index][lane], helper.sampleFrequency ? s : 0u);
                        }
                        QCOMPARE(observed[7][index][2] & 0x7fffffff, 0u);
                        QCOMPARE(observed[7][index][3] & 0x7fffffff, 0u);
                    }
            QVERIFY(comparisons > 0);
            d.validate();
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void fullSignature_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("packed");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool packed : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(packed))) << vendor << packed;
    }
    void fullSignature() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, packed);
        try {
            Device d(vendor);
            std::string inputs, sum;
            const std::string type = packed ? "float2" : "float4";
            for (unsigned i = 0; i < (packed ? 64u : 32u); ++i) {
                inputs += (i ? "," : "") + type + " a" + std::to_string(i) + ":DATA" + std::to_string(i);
                sum += (i ? "+a" : "a") + std::to_string(i);
            }
            const auto original = compile(type + " main(" + inputs + "):SV_Target{return " + sum + ";}");
            for (unsigned reg = 0; reg < 32; ++reg) {
                const auto helper = pixelInputShader(original, reg);
                QCOMPARE(helper.mask, 15u);
                unsigned fields = 0, mask = 0;
                for (const auto &f : helper.inputSignature)
                    if (f.reg == 0) {
                        QCOMPARE(f.semantic, std::string("DATA"));
                        QVERIFY(!(mask & f.mask));
                        mask |= f.mask;
                        ++fields;
                    }
                QCOMPARE(mask, 15u);
                QCOMPARE(fields, packed ? 2u : 1u);
                Com<ID3D11PixelShader> shader;
                check(d.device->CreatePixelShader(helper.bytecode.data(), helper.bytecode.size(), nullptr,
                                                  &shader),
                      "Create full-signature selected input PS");
            }
            d.validate();
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void geometryContract_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("factor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t factor : {1u, 2u, 3u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(factor))) << vendor << factor;
    }
    void geometryContract() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, factor);
        try {
            Device device(vendor);
            PostTransformGeometry geometry;
            nlohmann::json attrs = nlohmann::json::array();
            uint32_t stride = 0;
            auto attribute = [&](std::string name, uint32_t index, uint32_t system, uint32_t type,
                                 uint32_t count) {
                std::vector<uint32_t> components;
                for (uint32_t c = 0; c < count; ++c)
                    components.push_back(c);
                attrs.push_back({{"semantic", name},
                                 {"index", index},
                                 {"system_value", system},
                                 {"component_type", type},
                                 {"offset", stride},
                                 {"component_count", count},
                                 {"components", components}});
                stride += count * 4;
            };
            attribute("SV_Position", 0, 1, 3, 4);
            attribute("DATA", 0, 0, 3, 2);
            attribute("DATA", 1, 0, 3, 2);
            attribute("SV_ClipDistance", 0, 2, 3, 2);
            attribute("SV_CullDistance", 0, 3, 3, 2);
            attribute("SV_RenderTargetArrayIndex", 0, 4, 1, 1);
            attribute("SV_ViewportArrayIndex", 0, 5, 1, 1);
            geometry.report = {{"geometry_stage", "final_pre_rasterization"},
                               {"stride", stride},
                               {"vertices_per_primitive", factor},
                               {"primitives", 2},
                               {"vertices", 2 * factor},
                               {"attributes", attrs}};
            geometry.bytes.resize(2 * factor * stride);
            for (uint32_t i = 0; i < geometry.bytes.size() / 4; ++i) {
                const uint32_t value = 0x3f000000 + i;
                std::memcpy(geometry.bytes.data() + i * 4, &value, 4);
            }
            const auto ps = compile("float4 main(float4 p:SV_Position,float2 a:DATA0,float2 "
                                    "b:DATA1):SV_Target{return float4(a,b);}");
            const auto pixel = pixelInputShader(ps, inputRegister(ps, "DATA"));
            const auto bridge = pixelGeometryBridge(geometry, pixel, 1);
            QCOMPARE(bridge.verticesPerPrimitive, factor);
            QCOMPARE(bridge.identity.at("primitive_ordinal").get<uint64_t>(), uint64_t(1));
            std::set<uint32_t> systems;
            unsigned userFields = 0;
            for (const auto &field : bridge.identity.at("output_mapping")) {
                systems.insert(field.at("system_value").get<uint32_t>());
                if (field.at("system_value") == 0)
                    ++userFields;
                for (uint32_t vertex = 0; vertex < factor; ++vertex)
                    for (const auto &c : field.at("components")) {
                        const auto source =
                            (factor + vertex) * stride + c.at("source_byte_offset").get<uint32_t>();
                        const auto target = vertex * bridge.stride + field.at("slot").get<uint32_t>() * 16 +
                                            c.at("output_component").get<uint32_t>() * 4;
                        QCOMPARE(Reader(Bytes(bridge.vertices).subspan(target)).read<uint32_t>(),
                                 Reader(Bytes(geometry.bytes).subspan(source)).read<uint32_t>());
                    }
            }
            QCOMPARE(userFields, 2u);
            QVERIFY(systems == std::set<uint32_t>({0, 1, 2, 3, 4, 5}));
            Com<ID3D11GeometryShader> gs;
            check(device.device->CreateGeometryShader(bridge.geometryShader.data(),
                                                      bridge.geometryShader.size(), nullptr, &gs),
                  "Create rasterization-output bridge GS");
            const auto idPs =
                compile("uint4 main(float4 p:SV_Position,uint id:SV_PrimitiveID):SV_Target{return id;}");
            const auto idInput = pixelInputShader(idPs, inputRegister(idPs, "SV_PrimitiveID"));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryBridge(geometry, idInput, 1));
            const auto supplied = pixelGeometryBridge(geometry, idInput, 1, 42);
            check(device.device->CreateGeometryShader(supplied.geometryShader.data(),
                                                      supplied.geometryShader.size(), nullptr, &gs),
                  "Create explicit primitive-ID bridge GS");
            QCOMPARE(Reader(Bytes(supplied.vertices)).read<uint32_t>(), 42u);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryBridge(geometry, pixel, 2));
            auto malformed = geometry;
            malformed.bytes.pop_back();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryBridge(malformed, pixel, 0));
            malformed = geometry;
            malformed.report["attributes"][1]["semantic"] = "MISSING";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryBridge(malformed, pixel, 0));
            malformed = geometry;
            malformed.report["geometry_stage"] = "selected_shader_output";
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryBridge(malformed, pixel, 0));
            device.validate();
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void rejectsInvalidSelection() {
        const auto original = compile("float4 main(float4 a:DATA):SV_Target{return a;}");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelInputShader(original, 32));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelInputShader(original, 1));
        const auto vertex = compile("float4 main(float4 a:DATA):SV_Position{return a;}", "vs_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelInputShader(vertex, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelInputShader(Bytes{}, 0));
    }
    void preservesOtherSampleInterpolationTrigger() {
        const auto original = compile("float4 main(float4 p:SV_Position,linear float4 a:DATA,"
                                      "sample float4 b:TRIGGER):SV_Target{return a+b;}");
        const auto reg = inputRegister(original, "DATA");
        const auto helper = pixelInputShader(original, reg);
        QVERIFY(helper.sampleFrequency);
        bool originalInterpolation = false, sampleIndex = false;
        for (const auto &[tag, bytes] : readDxbcParts(helper.bytecode))
            if (tag == 0x58454853)
                for (const auto &row : readDxbcProgram(bytes).instructions) {
                    if ((row[0] & 2047) == 98 && row.at(2) == 0)
                        originalInterpolation = ((row[0] >> 11) & 15) == 2;
                    if ((row[0] & 2047) == 99 && row.back() == 10)
                        sampleIndex = true;
                }
        QVERIFY(originalInterpolation);
        QVERIFY(sampleIndex);
    }
    void rejectsReservedPrecision() {
        const auto original = compile("float4 main(min16float4 a:DATA):SV_Target{return float4(a);}");
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> owned;
        bool changed = false;
        for (const auto &[tag, bytes] : readDxbcParts(original)) {
            std::vector<uint8_t> copy(bytes.begin(), bytes.end());
            if (tag == 0x58454853) {
                auto program = readDxbcProgram(bytes);
                for (auto &row : program.instructions)
                    if ((row[0] & 2047) == 98 && (row.at(1) & 0x80000000)) {
                        row.at(2) = (row.at(2) & ~0x1c000u) | (3u << 14);
                        changed = true;
                    }
                copy = writeDxbcProgram(program);
            }
            owned.emplace_back(tag, std::move(copy));
        }
        QVERIFY(changed);
        DxbcParts parts;
        for (const auto &[tag, bytes] : owned)
            parts.emplace_back(tag, bytes);
        const auto malformed = makeDxbc(parts);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelInputShader(malformed, 0));
    }
};
QTEST_GUILESS_MAIN(DxbcPixelInputsTests)
#include "DxbcPixelInputsTests.moc"
