#include "application/DxbcExecutionTraceReader.h"
#include "application/DxbcGpuCompletion.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcPixelGeometry.h"
#include "application/DxbcPixelInputs.h"
#include "application/DxbcPixelRaster.h"
#include "application/HlslCompilation.h"
#include "application/NativePixelInvocation.h"
#include "application/NativePixelOutputs.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
    void positionInterpolation_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<uint32_t>("sample");
        QTest::addColumn<bool>("edge");
        QTest::addColumn<bool>("positionFirst");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 4u})
                for (uint32_t sample = 0; sample < samples; ++sample)
                    for (bool edge : {false, true}) {
                        if (edge && (samples == 1 || (sample != 0 && sample != 2)))
                            continue;
                        for (bool positionFirst : {false, true})
                            for (const auto *mode : {"center", "centroid", "sample", "attribute-sample"})
                                QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5-%6")
                                                             .arg(vendor)
                                                             .arg(samples)
                                                             .arg(sample)
                                                             .arg(edge)
                                                             .arg(positionFirst)
                                                             .arg(mode)))
                                    << vendor << samples << sample << edge << positionFirst << QString(mode);
                    }
    }
    void positionInterpolation() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, samples);
        QFETCH(uint32_t, sample);
        QFETCH(bool, edge);
        QFETCH(bool, positionFirst);
        QFETCH(QString, mode);
        using Json = nlohmann::json;
        Json report{{"schema", 1},
                    {"kind", "native_position_interpolation_probe"},
                    {"vendor_request", vendor},
                    {"samples", samples},
                    {"sample", sample},
                    {"edge", edge},
                    {"position_first", positionFirst},
                    {"mode", mode.toStdString()},
                    {"status", "pending"},
                    {"original_fragment_eligibility", nullptr}};
        std::string failure;
        try {
            Device d(vendor);
            const auto vs = compile(std::string("struct O{") +
                                        (positionFirst ? "float4 p:SV_Position;float4 a:DATA;"
                                                       : "float4 a:DATA;float4 p:SV_Position;") +
                                        "};O main(uint i:SV_VertexID){O o;o.p=float4(i==1?1:-1,i==2?-1:1,"
                                        "i==0?.125:i==1?.875:.625,1);o.a=float4(i,i+1,i+2,i+3);return o;}",
                                    "vs_5_0");
            Com<ID3D11VertexShader> vertex;
            check(d.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex), "Create position VS");
            d.context->VSSetShader(vertex.Get(), nullptr, 0);
            const auto geometry = captureGeometry(d, vs);
            Target target;
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
            check(d.device->CreateTexture2D(&td, nullptr, &target.texture), "Create position target");
            check(d.device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv),
                  "Create position RTV");
            check(d.device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv),
                  "Create position SRV");
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            raster.MultisampleEnable = samples > 1;
            Com<ID3D11RasterizerState> rs;
            check(d.device->CreateRasterizerState(&raster, &rs), "Create position rasterizer");
            d.context->RSSetState(rs.Get());
            const D3D11_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
            d.context->RSSetViewports(1, &viewport);
            const std::string position =
                (mode == "center" || mode == "attribute-sample" ? "" : mode.toStdString() + " ") +
                std::string("noperspective float4 p:SV_Position");
            const std::string data = mode == "attribute-sample" ? "sample float4 a:DATA" : "float4 a:DATA";
            const auto source = "uint4 main(" +
                                (positionFirst ? position + "," + data : data + "," + position) +
                                "):SV_Target{return uint4(asuint(p.z),asuint(a.x),asuint(a.y),asuint(p.x));}";
            const auto original = compile(source);
            report["source"] = source;
            report["shader_sha256"] = sha256(original);
            Com<ID3DBlob> disassembly;
            check(D3DDisassemble(original.data(), original.size(), 0, nullptr, &disassembly),
                  "Disassemble original position PS");
            report["shader_disassembly"] =
                std::string(static_cast<const char *>(disassembly->GetBufferPointer()));
            Com<ID3D11PixelShader> shader;
            check(d.device->CreatePixelShader(original.data(), original.size(), nullptr, &shader),
                  "Create position PS");
            d.context->PSSetShader(shader.Get(), nullptr, 0);
            d.context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
            const float clear[4]{};
            d.context->ClearRenderTargetView(target.rtv.Get(), clear);
            d.context->Draw(3, 0);
            d.context->OMSetRenderTargets(0, nullptr, nullptr);
            const auto raw = read(d, target, samples);
            d.validate();
            d.context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
            const uint32_t x = edge ? 7 : 3, y = edge ? 8 : 2;
            const auto expected = raw[(y * 16 + x) * samples + sample];
            report["reference"] = expected;
            report["x"] = x;
            report["y"] = y;
            const auto positionReg = inputRegister(original, "SV_Position");
            const auto dataReg = inputRegister(original, "DATA");
            report["position_register"] = positionReg;
            report["data_register"] = dataReg;
            if ((mode == "sample" || mode == "attribute-sample") && samples == 4) {
                auto observe = [&] {
                    d.context->ClearRenderTargetView(target.rtv.Get(), clear);
                    d.context->Draw(3, 0);
                    d.context->OMSetRenderTargets(0, nullptr, nullptr);
                    const auto bytes = read(d, target, samples);
                    d.context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
                    d.validate();
                    return bytes[(y * 16 + x) * samples + sample];
                };
                const auto indexedSource = "uint4 main(" +
                                           (positionFirst ? position + "," + data : data + "," + position) +
                                           ",uint s:SV_SampleIndex):SV_Target{return "
                                           "uint4(asuint(p.z),asuint(a.x),asuint(a.y),asuint(p.x)+s);}";
                const auto indexedCode = compile(indexedSource);
                Com<ID3D11PixelShader> indexedShader;
                check(d.device->CreatePixelShader(indexedCode.data(), indexedCode.size(), nullptr,
                                                  &indexedShader),
                      "Create independent indexed position PS");
                d.context->PSSetShader(indexedShader.Get(), nullptr, 0);
                auto indexedValue = observe();
                indexedValue[3] -= sample;
                report["explicit_sample_index_reference"] = indexedValue;
                d.context->PSSetShader(shader.Get(), nullptr, 0);
                const auto gsSource =
                    std::string("struct O{") +
                    (positionFirst ? "float4 p:SV_Position;float4 a:DATA;"
                                   : "float4 a:DATA;float4 p:SV_Position;") +
                    "};[maxvertexcount(3)]void main(triangle O input[3],inout TriangleStream<O> output){"
                    "output.Append(input[0]);output.Append(input[1]);output.Append(input[2]);}";
                const auto gsCode = compile(gsSource, "gs_5_0");
                Com<ID3D11GeometryShader> passthrough;
                check(d.device->CreateGeometryShader(gsCode.data(), gsCode.size(), nullptr, &passthrough),
                      "Create independent position GS");
                d.context->GSSetShader(passthrough.Get(), nullptr, 0);
                report["geometry_shader_reference"] = observe();
                d.context->GSSetShader(nullptr, nullptr, 0);
            }
            const auto invocation = captureNativePixelInvocationInputs(
                d.device.Get(), !vendor, pixelRasterState(d.context.Get(), 0), geometry, original, 0,
                {x, y, sample, 0, 2, false});
            const auto &p = invocation.registers[invocation.selectedLane].at({1, {positionReg}});
            const auto &a = invocation.registers[invocation.selectedLane].at({1, {dataReg}});
            report["position_bits"] = p.bits;
            report["position_valid"] = p.valid;
            report["data_bits"] = a.bits;
            report["data_valid"] = a.valid;
            report["provenance"] = invocation.inputProvenance;
            if ((mode == "sample" || mode == "attribute-sample") && samples == 4 && sample == 3 && !edge &&
                positionFirst) {
                auto expanded = geometry;
                expanded.bytes.insert(expanded.bytes.end(), geometry.bytes.begin(), geometry.bytes.end());
                expanded.report["vertices"] = 6;
                expanded.report["primitives"] = 2;
                const auto capture = pixelInputShader(original, positionReg,
                                                      PixelInputRecordSelection{x, y, sample, 0, 2, false});
                const auto range = pixelGeometryRangeBridge(expanded, capture, 0, 2);
                const auto state = pixelRasterState(d.context.Get(), 0);
                const auto observed =
                    capturePixelRasterInputs(d.device.Get(), !vendor, state, range, capture);
                report["range_observation"] = observed.identity;
                if (observed.records.size() != 2 || observed.identity.at("private_draws") != 2)
                    throw std::runtime_error("Implicit sample geometry range lost records");
                for (const auto &record : observed.records)
                    if (record.words[record.words[4][2] + 2 * record.words[4][3]] != p.bits)
                        throw std::runtime_error("Implicit sample geometry range input differs");
                auto masked = state;
                masked.sampleMask = 0;
                if (!capturePixelRasterInputs(d.device.Get(), !vendor, masked, range, capture)
                         .records.empty())
                    throw std::runtime_error("Masked implicit sample retained a record");
                QVERIFY_THROWS_EXCEPTION(
                    std::runtime_error,
                    capturePixelRasterInputs(d.device.Get(), !vendor, state, range, capture, 1));
                unsigned checks = 0;
                QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                         capturePixelRasterInputs(d.device.Get(), !vendor, state, range,
                                                                  capture, 1024 * 1024,
                                                                  [&] { return ++checks >= 8; }));
                report["range_recovery_checks"] = checks;
                const auto retried = capturePixelRasterInputs(d.device.Get(), !vendor, state, range, capture);
                if (retried.records.size() != observed.records.size())
                    throw std::runtime_error("Implicit sample retry lost records");
                for (size_t i = 0; i < observed.records.size(); ++i)
                    if (retried.records[i].words != observed.records[i].words)
                        throw std::runtime_error("Implicit sample retry changed input values");
                Com<ID3D11PixelShader> boundShader;
                Com<ID3D11RenderTargetView> boundTarget;
                d.context->PSGetShader(&boundShader, nullptr, nullptr);
                d.context->OMGetRenderTargets(1, &boundTarget, nullptr);
                if (boundShader.Get() != shader.Get() || boundTarget.Get() != target.rtv.Get())
                    throw std::runtime_error("Implicit sample diagnostics changed original bindings");
                d.context->Draw(3, 0);
                d.context->OMSetRenderTargets(0, nullptr, nullptr);
                const auto postCancel = read(d, target, samples)[(y * 16 + x) * samples + sample];
                d.context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
                report["post_cancel_reference"] = postCancel;
                report["retry_records"] = retried.records.size();
                if (postCancel != expected)
                    throw std::runtime_error("Original drawing changed after implicit sample cancellation");
            }
            if (positionReg == dataReg || p.valid != 5 || a.valid != 3 ||
                Words{p.bits[2], a.bits[0], a.bits[1], p.bits[0]} != expected ||
                invocation.inputProvenance.at("selected_raster").at("depth_bits") != expected[0])
                throw std::runtime_error("Original GPU position/data inputs differ");
            d.validate();
            report["status"] = "ok";
        } catch (const std::exception &error) {
            failure = error.what();
            report["status"] = "failed";
            report["error"] = failure;
        }
        const auto root = qEnvironmentVariable("FLORA_POSITION_INTERPOLATION_EVIDENCE");
        if (!root.isEmpty()) {
            QVERIFY(QDir().mkpath(root));
            QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto text = report.dump(2);
            QCOMPARE(file.write(text.data(), qint64(text.size())), qint64(text.size()));
        }
        QVERIFY2(failure.empty(), failure.c_str());
    }
    void mergerInputs_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("samples");
        QTest::addColumn<bool>("reverse");
        QTest::addColumn<bool>("sloped");
        QTest::addColumn<QString>("mode");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t samples : {1u, 4u})
                for (bool reverse : {false, true})
                    for (bool sloped : {false, true})
                        for (const auto *mode : {"constant", "position", "front", "sample"})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(samples)
                                                         .arg(reverse)
                                                         .arg(sloped)
                                                         .arg(mode)))
                                << vendor << samples << reverse << sloped << QString(mode);
    }
    void mergerInputs() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, samples);
            QFETCH(bool, reverse);
            QFETCH(bool, sloped);
            QFETCH(QString, mode);
            Device d(vendor);
            const auto vs =
                compile(std::string("struct O{float4 p:SV_Position;float a:DATA;};"
                                    "O main(uint id:SV_VertexID){O o;o.p=float4(id==1?3:-1,id==2?-3:1,") +
                            (sloped ? "id==0?.125:id==1?.875:.625" : ".25") + ",1);o.a=id;return o;}",
                        "vs_5_0");
            Com<ID3D11VertexShader> vertex;
            check(d.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex),
                  "Create raster-depth VS");
            d.context->VSSetShader(vertex.Get(), nullptr, 0);
            auto geometry = captureGeometry(d, vs);
            Target target;
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
            check(d.device->CreateTexture2D(&td, nullptr, &target.texture), "Create raster-depth reference");
            check(d.device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv),
                  "Create raster-depth RTV");
            check(d.device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv),
                  "Create raster-depth SRV");
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            raster.MultisampleEnable = samples > 1;
            raster.FrontCounterClockwise = reverse;
            Com<ID3D11RasterizerState> rs;
            check(d.device->CreateRasterizerState(&raster, &rs), "Create raster-depth state");
            d.context->RSSetState(rs.Get());
            const D3D11_VIEWPORT viewport{0, 0, 16, 16, .2f, .8f};
            d.context->RSSetViewports(1, &viewport);
            const bool sampled = mode == "sample";
            const auto reference = compile(
                std::string(
                    "uint4 main(float4 p:SV_Position,bool front:SV_IsFrontFace,uint coverage:SV_Coverage") +
                (sampled ? ",uint sample:SV_SampleIndex" : "") +
                "):SV_Target{return uint4(asuint(p.z),front?1:0,coverage," + (sampled ? "sample" : "0") +
                ");}");
            Com<ID3D11PixelShader> shader;
            check(d.device->CreatePixelShader(reference.data(), reference.size(), nullptr, &shader),
                  "Create independent raster-depth PS");
            d.context->PSSetShader(shader.Get(), nullptr, 0);
            d.context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
            d.context->Draw(3, 0);
            d.context->OMSetRenderTargets(0, nullptr, nullptr);
            const auto raw = read(d, target, samples);
            d.context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
            const auto state = pixelRasterState(d.context.Get(), 0);
            const auto original = compile(
                mode == "constant"   ? "uint4 main():SV_Target{return uint4(1,2,3,4);}"
                : mode == "position" ? "float4 main(float4 p:SV_Position):SV_Target{return p.x;}"
                : mode == "front"
                    ? "uint4 main(bool f:SV_IsFrontFace):SV_Target{return f?1:0;}"
                    : "float4 main(sample float a:DATA,uint s:SV_SampleIndex):SV_Target{return a+s;}");
            const auto invocation = captureNativePixelInvocationInputs(
                d.device.Get(), !vendor, state, geometry, original, 0, {3, 2, samples - 1, 0, 1, false});
            const auto &observed = invocation.inputProvenance.at("selected_raster");
            const auto expected = raw[(2 * 16 + 3) * samples + samples - 1];
            QCOMPARE(observed.at("depth_bits"), nlohmann::json(expected[0]));
            QCOMPARE(observed.at("front_face"), nlohmann::json(expected[1] != 0));
            QCOMPARE(observed.at("coverage_mask"), nlohmann::json(expected[2]));
            QCOMPARE(observed.at("sample"), nlohmann::json(samples - 1));
            QCOMPARE(observed.at("lane"), nlohmann::json(invocation.selectedLane));
            QCOMPARE(expected[1], uint32_t(!reverse));
            QCOMPARE(expected[3], sampled ? samples - 1 : 0u);
            QCOMPARE(invocation.inputProvenance.at("sample_frequency"), nlohmann::json(sampled));
            d.validate();
            const auto root = qEnvironmentVariable("FLORA_RASTER_MERGER_INPUTS_EVIDENCE");
            if (!root.isEmpty()) {
                if (!QDir().mkpath(root))
                    throw std::runtime_error("Cannot create raster merger evidence directory");
                QFile file(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
                const auto text = nlohmann::json{
                    {"schema", 1},
                    {"kind", "private_raster_merger_inputs_probe"},
                    {"vendor_request", vendor},
                    {"samples", samples},
                    {"reverse", reverse},
                    {"sloped", sloped},
                    {"mode", mode.toStdString()},
                    {"reference", expected},
                    {"original_shader_sha256", sha256(original)},
                    {"reference_shader_sha256", sha256(reference)},
                    {"provenance", invocation.inputProvenance},
                    {"original_fragment_eligibility",
                     nullptr}}.dump(2);
                if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                    file.write(text.data(), qint64(text.size())) != qint64(text.size()))
                    throw std::runtime_error("Cannot save raster merger input evidence");
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void capture_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<uint32_t>("samples");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *mode : {"linear",
                                     "noperspective",
                                     "centroid",
                                     "noperspective centroid",
                                     "sample",
                                     "noperspective sample",
                                     "nointerpolation",
                                     "uint",
                                     "sint",
                                     "sparse",
                                     "min16",
                                     "trigger",
                                     "position",
                                     "position-z",
                                     "sample-id",
                                     "sm4",
                                     "sm41",
                                     "eval-centroid",
                                     "eval-sample",
                                     "eval-snapped",
                                     "eval-snapped-high",
                                     "eval-centroid-noperspective",
                                     "eval-sample-noperspective",
                                     "eval-snapped-noperspective",
                                     "eval-centroid-constant",
                                     "eval-sample-constant",
                                     "eval-snapped-constant",
                                     "eval-centroid-frequency",
                                     "eval-sample-frequency",
                                     "eval-snapped-frequency",
                                     "eval-centroid-edge",
                                     "eval-sample-edge",
                                     "eval-snapped-edge"})
                for (uint32_t samples : {1u, 4u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(mode).arg(samples)))
                        << vendor << QString(mode) << samples;
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *operation : {"centroid", "sample", "snapped"})
                for (const auto *precision : {"min16", "min10"})
                    for (const auto *suffix : {"", "-noperspective", "-constant", "-frequency", "-edge"})
                        for (uint32_t samples : {1u, 4u}) {
                            const auto mode =
                                QString("eval-%1-%2%3").arg(operation).arg(precision).arg(suffix);
                            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(mode).arg(samples)))
                                << vendor << mode << samples;
                        }
    }
    void attributeRequestValidation() {
        const auto shader = compile("uint4 main(float4 p:SV_Position):SV_Target{return asuint(p);}");
        const auto reg = inputRegister(shader, "SV_Position");
        QVERIFY_EXCEPTION_THROWN(pixelInputShader(shader, reg, {}, PixelAttributeEvaluation{}),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(pixelInputShader(shader, reg, {}, PixelAttributeEvaluation{202, {}}),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(pixelInputShader(shader, reg, {}, PixelAttributeEvaluation{205, {1, 0}}),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(pixelInputShader(shader, reg, {}, PixelAttributeEvaluation{204, {0, 1}}),
                                 std::runtime_error);
        for (const auto precision : {std::array<uint32_t, 2>{3, 0}, {0, 3}, {4, 0}, {0, 5}})
            QVERIFY_EXCEPTION_THROWN(
                pixelInputShader(shader, reg, {}, PixelAttributeEvaluation{205, {}, precision}),
                std::runtime_error);
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
            std::optional<PixelAttributeEvaluation> evaluation;
            std::string evaluated = "v";
            if (mode.startsWith("eval-")) {
                evaluation.emplace();
                if (mode.startsWith("eval-centroid")) {
                    evaluated = "EvaluateAttributeCentroid(v)";
                } else if (mode.startsWith("eval-sample")) {
                    evaluation->opcode = 204;
                    evaluation->location[0] = samples - 1;
                    evaluated = "EvaluateAttributeAtSample(v," + std::to_string(samples - 1) + ")";
                } else {
                    evaluation->opcode = 203;
                    evaluation->location = mode.endsWith("high")
                                               ? std::array<uint32_t, 2>{0x12345678, 0xfffffff7}
                                               : std::array<uint32_t, 2>{0xfffffff8, 7};
                    evaluated = "EvaluateAttributeSnapped(v,int2(-8,7))";
                }
            }
            std::string qualifier = "linear", type = "float4";
            // The installed compiler rejects eval_* on constant declarations.
            // Observe the independently specified identity operation instead;
            // the product helper must preserve the provoking vertex's value.
            if (mode.endsWith("constant"))
                evaluated = "v";
            if (integer) {
                qualifier = "nointerpolation";
                type = mode == "uint" ? "uint4" : "int4";
            } else if (mode == "min16")
                type = "min16float4";
            else if (evaluation)
                qualifier = mode.endsWith("noperspective") ? "noperspective"
                            : mode.endsWith("constant")    ? "nointerpolation"
                            : mode.endsWith("frequency")   ? "sample"
                                                           : "centroid";
            else if (!system && !sparse && mode != "trigger" && mode != "sm4" && mode != "sm41")
                qualifier = mode.toStdString();
            if (evaluation && (mode.contains("-min16") || mode.contains("-min10"))) {
                const uint32_t precision = mode.contains("-min16") ? 1 : 2;
                type = precision == 1 ? "min16float4" : "min10float4";
                evaluation->precision = {precision, precision};
                evaluated = "float4(" + type + "(" + evaluated + "))";
            }
            const std::string value = integer ? (mode == "uint" ? "uint4(0x80000000,0x7fc12345,0xffffffff,1)"
                                                                : "int4(-123,-2147483647,12345,-1)")
                                      : mode.contains("-min10")
                                          ? "float4(.125+id*.03125,-.25+id*.0625,.375+id*.125,-.75+id*.25)"
                                          : "float4(id*3.125+0.125,id*0.5-0.25,id*1.25+0.375,id*4.5-2.75)";
            const std::string vsBody =
                "O main(uint id:SV_VertexID){O o;float2 p=" +
                std::string(mode.endsWith("edge")
                                ? "id==0?float2(-.65,.7):id==1?float2(-.45,.7):float2(-.65,.5);"
                                : "id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);") +
                "float w=id==0?1:id==1?2:4;o.p=float4(p*w,0.25*w,w);o.v=" + value + ";return o;}";
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
                                       : evaluated) +
                    ");o.sample=" + (mode == "trigger" ? "s" : "0") + ";return o;}";
            auto ps = compile(psSource, mode == "sm4" ? "ps_4_0" : mode == "sm41" ? "ps_4_1" : "ps_5_0");
            const auto selected = inputRegister(ps, mode.startsWith("position") ? "SV_Position"
                                                    : mode == "sample-id"       ? "SV_SampleIndex"
                                                                                : "DATA");
            const auto helper = pixelInputShader(ps, selected, {}, evaluation);
            QCOMPARE(helper.mask, mode == "position-z" ? 4u : mode == "sample-id" ? 1u : sparse ? 5u : 15u);
            QCOMPARE(helper.sampleFrequency, mode == "sample" || mode == "noperspective sample" ||
                                                 mode == "trigger" || mode == "sample-id" ||
                                                 mode.endsWith("frequency"));
            Com<ID3D11VertexShader> vertex, compactVertex;
            const std::string parameters =
                "float4 p:SV_Position" + (system ? std::string() : "," + qualifier + " " + type + " v:DATA") +
                (helper.sampleFrequency ? ",uint s:SV_SampleIndex" : "");
            const std::string raw = mode == "sample-id"    ? "uint4(s,0,0,0)"
                                    : mode == "position"   ? "asuint(p)"
                                    : mode == "position-z" ? "asuint(float4(0,0,p.z,0))"
                                    : sparse               ? "asuint(float4(v.x,0,v.z,0))"
                                    : mode == "min16"      ? "asuint(float4(v))"
                                                           : "asuint(" + evaluated + ")";
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
            d.context->OMSetRenderTargets(1, targets[0].rtv.GetAddressOf(), nullptr);
            const auto rasterState = pixelRasterState(d.context.Get(), 0);
            const PixelInputRecordSelection recordSelection{3, 2, samples - 1, 0, 32, false};
            const auto recordShader = pixelInputShader(ps, selected, recordSelection, evaluation);
            const auto recordBridge = pixelGeometryBridge(geometry, recordShader, 0);
            const auto recordCapture = capturePixelRasterInputs(d.device.Get(), vendor == 0, rasterState,
                                                                recordBridge, recordShader);
            QCOMPARE(recordCapture.records.size(), size_t(1));
            const auto address = (2 * 16 + 3) * samples + samples - 1;
            for (unsigned i = 0; i < 8; ++i) {
                if (i == 5 && recordShader.sampleRecordTarget) {
                    // No SV_SampleIndex was declared or injected. These words
                    // remain unavailable; the integer RTV identifies storage.
                    QCOMPARE(recordCapture.identity.at("sample_index_observed"), nlohmann::json(false));
                    QCOMPARE(recordCapture.records[0].words[i], Words{});
                } else
                    QCOMPARE(recordCapture.records[0].words[i], observed[i][address]);
            }
            const auto &selectedRecord = recordCapture.records[0].words;
            const auto selectedLane = selectedRecord[4][2] + 2 * selectedRecord[4][3];
            QCOMPARE(selectedRecord[selectedLane], expected[address]);
            nlohmann::json nativeExecution;
            QByteArray nativeJournal;
            if (evaluation) {
                const auto cancelled = std::make_shared<bool>(false);
                auto invocation = captureNativePixelInvocationInputs(d.device.Get(), vendor == 0, rasterState,
                                                                     geometry, ps, 0, recordSelection, {},
                                                                     [cancelled] { return *cancelled; });
                if (mode.endsWith("edge"))
                    QVERIFY(invocation.activeMask && invocation.activeMask != 15);
                InputSnapshot snapshot;
                snapshot.stage = 4;
                snapshot.event = 1;
                snapshot.shader = 2;
                snapshot.frameSha256 = sha256(vs);
                snapshot.bytecode = ps;
                if (evaluation->precision[0] || evaluation->precision[1])
                    snapshot.inputs.gpu = std::make_shared<GpuOperations>(d.device.Get(), vendor == 0);
                snapshot.adapterVendor = recordCapture.identity.at("adapter_vendor");
                snapshot.adapterDevice = recordCapture.identity.at("adapter_device");
                snapshot.adapterLuid = recordCapture.identity.at("adapter_luid");
                nativeExecution = evaluateNativePixelOutputs(snapshot, invocation);
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const auto &outputs = nativeExecution.at("lanes")[lane].at("outputs");
                    const auto color = std::find_if(outputs.begin(), outputs.end(), [](const auto &value) {
                        return value.at("kind") == "color" && value.at("index") == 0;
                    });
                    QVERIFY(color != outputs.end());
                    QCOMPARE(color->at("valid_mask"), nlohmann::json(15));
                    QCOMPARE(color->at("raw_bits"), nlohmann::json(neighbors[lane][address]));
                }
                QTemporaryDir temporary;
                QVERIFY(temporary.isValid());
                const auto path = temporary.filePath("attribute-trace.jsonl");
                const auto completion = exportPixelExecutionTrace(path, snapshot, invocation);
                QFile journal(path);
                QVERIFY(journal.open(QIODevice::ReadOnly));
                nativeJournal = journal.readAll();
                journal.close();
                ExecutionTraceReader reader(
                    path, QCryptographicHash::hash(nativeJournal, QCryptographicHash::Sha256)
                              .toHex()
                              .toStdString());
                reader.moveTo(reader.size());
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const auto value = reader.registerValue(lane, {2, {0}});
                    QCOMPARE(value.valid, uint8_t(15));
                    QCOMPARE(value.bits, neighbors[lane][address]);
                }
                reader.moveTo(0);
                reader.moveTo(reader.size());
                nativeExecution["trace_completion"] = completion;
                nativeExecution["input_provenance"] = invocation.inputProvenance;
                if (invocation.attributeObservations) {
                    QCOMPARE(invocation.attributeObservations->size(), size_t(1));
                    const AttributeRequest outside{204, selected, {samples, 0}, evaluation->precision};
                    const auto unknown = invocation.evaluateAttribute(outside);
                    for (const auto &value : unknown) {
                        QCOMPARE(value.valid, uint8_t(0));
                        QCOMPARE(value.bits, Words{});
                    }
                    QCOMPARE(invocation.attributeObservations->size(), size_t(2));
                    QCOMPARE(invocation.evaluateAttribute(outside), unknown);
                    QCOMPARE(invocation.attributeObservations->size(), size_t(2));
                    nativeExecution["undefined_sample"] = invocation.attributeObservations->back();
                    *cancelled = true;
                    QVERIFY_EXCEPTION_THROWN(invocation.evaluateAttribute(outside), OperationCancelled);
                    nativeExecution["cancelled_cache_hit_rejected"] = true;
                }
            }
            Com<ID3D11RenderTargetView> stillBound;
            d.context->OMGetRenderTargets(1, &stillBound, nullptr);
            QCOMPARE(stillBound.Get(), targets[0].rtv.Get());
            d.context->OMSetRenderTargets(0, nullptr, nullptr);
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
                shader("record-ps", recordShader.bytecode);
                save("records.bin", recordCapture.storage.data(), recordCapture.storage.size());
                const auto recordIdentity = recordCapture.identity.dump(2);
                save("records.json", recordIdentity.data(), recordIdentity.size());
                save("source-geometry.bin", geometry.bytes.data(), geometry.bytes.size());
                save("bridge-geometry.bin", bridge.vertices.data(), bridge.vertices.size());
                const auto sourceGeometry = geometry.report.dump(2), bridgeIdentity = bridge.identity.dump(2);
                save("source-geometry.json", sourceGeometry.data(), sourceGeometry.size());
                save("bridge-geometry.json", bridgeIdentity.data(), bridgeIdentity.size());
                shader("original-ps", ps);
                if (evaluation) {
                    const auto text = nativeExecution.dump(2);
                    save("native-execution.json", text.data(), text.size());
                    save("native-trace.jsonl", nativeJournal.data(), size_t(nativeJournal.size()));
                }
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
                        const Words untouched{16777216, 16777216, 16777216, 16777216};
                        if (mode.endsWith("edge") && location == untouched) {
                            QCOMPARE(expected[index], untouched);
                            for (unsigned lane = 0; lane < 4; ++lane)
                                QCOMPARE(neighbors[lane][index], untouched);
                            for (unsigned target = 0; target < 8; ++target)
                                QCOMPARE(observed[target][index], untouched);
                            continue;
                        }
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
                const auto recorded =
                    pixelInputShader(original, reg, PixelInputRecordSelection{1, 1, 0, 0, 1, false});
                check(d.device->CreatePixelShader(recorded.bytecode.data(), recorded.bytecode.size(), nullptr,
                                                  &shader),
                      "Create full-signature private record PS");
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
            const auto range = pixelGeometryRangeBridge(geometry, pixel, 0, 2);
            auto concatenated = pixelGeometryBridge(geometry, pixel, 0).vertices;
            concatenated.insert(concatenated.end(), bridge.vertices.begin(), bridge.vertices.end());
            QCOMPARE(range.vertices, concatenated);
            QCOMPARE(range.vertexShader, bridge.vertexShader);
            QCOMPARE(range.geometryShader, bridge.geometryShader);
            QCOMPARE(range.identity.at("primitive_count"), nlohmann::json(2));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryRangeBridge(geometry, pixel, 0, 0));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelGeometryRangeBridge(geometry, pixel, 1, 2));
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
    void rasterContract_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void rasterContract() {
        QFETCH(uint32_t, vendor);
        try {
            Device d(vendor);
            const auto vs = compile("struct O{float4 p:SV_Position;float4 a:DATA;};"
                                    "O main(uint id:SV_VertexID){O o;o.p=float4(id==1?3:-1,id==2?-3:1,0.5,1);"
                                    "o.a=float4(1,2,3,4);return o;}",
                                    "vs_5_0");
            Com<ID3D11VertexShader> vertex;
            check(d.device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex),
                  "Create raster fixture VS");
            d.context->VSSetShader(vertex.Get(), nullptr, 0);
            auto geometry = captureGeometry(d, vs);
            // The SO fixture has no layer output. Add an explicit constant layer
            // to the synthetic final geometry so the private GS must forward it.
            const auto stride = geometry.report.at("stride").get<uint32_t>();
            std::vector<uint8_t> layered(3 * (stride + 4));
            for (unsigned i = 0; i < 3; ++i) {
                std::memcpy(layered.data() + i * (stride + 4), geometry.bytes.data() + i * stride, stride);
                const uint32_t layer = 1;
                std::memcpy(layered.data() + i * (stride + 4) + stride, &layer, 4);
            }
            geometry.bytes = std::move(layered);
            geometry.report["stride"] = stride + 4;
            geometry.report["attributes"].push_back({{"semantic", "SV_RenderTargetArrayIndex"},
                                                     {"index", 0},
                                                     {"system_value", 4},
                                                     {"component_type", 1},
                                                     {"register", 2},
                                                     {"mask", 1},
                                                     {"used_mask", 1},
                                                     {"components", {0}},
                                                     {"register_components", {0}},
                                                     {"offset", stride},
                                                     {"component_count", 1}});
            D3D11_TEXTURE2D_DESC td{32,
                                    32,
                                    2,
                                    4,
                                    DXGI_FORMAT_R8G8B8A8_UNORM,
                                    {1, 0},
                                    D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET,
                                    0,
                                    0};
            Com<ID3D11Texture2D> texture;
            check(d.device->CreateTexture2D(&td, nullptr, &texture), "Create raster array target");
            D3D11_RENDER_TARGET_VIEW_DESC vd{};
            vd.Format = td.Format;
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray = {1, 1, UINT32_MAX};
            Com<ID3D11RenderTargetView> view;
            check(d.device->CreateRenderTargetView(texture.Get(), &vd, &view), "Create raster subset view");
            d.context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
            const D3D11_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
            d.context->RSSetViewports(1, &viewport);
            auto state = pixelRasterState(d.context.Get(), 0);
            QCOMPARE(state.target.width, 16u);
            QCOMPARE(state.target.height, 16u);
            QCOMPARE(state.target.layers, 3u);
            state.rasterizer.CullMode = D3D11_CULL_NONE;
            const auto ps = compile("float4 main(float4 a:DATA):SV_Target{return a;}");
            const auto reg = inputRegister(ps, "DATA");
            const PixelInputRecordSelection selection{3, 2, 0, 1, 1, true};
            const auto input = pixelInputShader(ps, reg, selection);
            const auto bridge = pixelGeometryBridge(geometry, input, 0);
            const auto observed = capturePixelRasterInputs(d.device.Get(), vendor == 0, state, bridge, input);
            QCOMPARE(observed.records.size(), size_t(1));
            const auto &record = observed.records[0].words;
            const auto lane = record[4][2] + 2 * record[4][3];
            const Words expected{0x3f800000, 0x40000000, 0x40400000, 0x40800000};
            QCOMPARE(record[lane], expected);
            QCOMPARE(record[8][0], 1u);
            auto empty = state;
            empty.sampleMask = 0;
            QVERIFY(
                capturePixelRasterInputs(d.device.Get(), vendor == 0, empty, bridge, input).records.empty());
            empty = state;
            empty.rasterizer.ScissorEnable = true;
            empty.scissors = {{8, 8, 16, 16}};
            QVERIFY(
                capturePixelRasterInputs(d.device.Get(), vendor == 0, empty, bridge, input).records.empty());
            empty.scissors.clear();
            QVERIFY(
                capturePixelRasterInputs(d.device.Get(), vendor == 0, empty, bridge, input).records.empty());
            auto wrongLayer = selection;
            wrongLayer.layer = 0;
            const auto other = pixelInputShader(ps, reg, wrongLayer);
            QVERIFY(
                capturePixelRasterInputs(d.device.Get(), vendor == 0, state, bridge, other).records.empty());
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturePixelRasterInputs(d.device.Get(), vendor == 0,
                                                                                  state, bridge, input, 1));
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     capturePixelRasterInputs(d.device.Get(), vendor == 0, state, bridge,
                                                              input, 1024 * 1024, [] { return true; }));
            unsigned cancellationChecks = 0;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled,
                                     capturePixelRasterInputs(d.device.Get(), vendor == 0, state, bridge,
                                                              input, 1024 * 1024,
                                                              [&] { return ++cancellationChecks >= 4; }));
            QVERIFY(cancellationChecks >= 4);
            auto missingLayer = selection;
            missingLayer.layerInput = false;
            const auto missing = pixelInputShader(ps, reg, missingLayer);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturePixelRasterInputs(d.device.Get(), vendor == 0,
                                                                                  state, bridge, missing));
            auto invalid = state;
            invalid.target.width = 3;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, capturePixelRasterInputs(d.device.Get(), vendor == 0,
                                                                                  invalid, bridge, input));
            const auto retried = capturePixelRasterInputs(d.device.Get(), vendor == 0, state, bridge, input);
            QVERIFY(retried.storage == observed.storage);
            Com<ID3D11RenderTargetView> stillBound;
            d.context->OMGetRenderTargets(1, &stillBound, nullptr);
            QCOMPARE(stillBound.Get(), view.Get());
            d.context->OMSetRenderTargets(0, nullptr, nullptr);
            // A 3D view's all-remaining-slices sentinel is relative to its mip.
            const D3D11_TEXTURE3D_DESC volumeDesc{
                32, 16, 8, 2, td.Format, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
            Com<ID3D11Texture3D> volume;
            check(d.device->CreateTexture3D(&volumeDesc, nullptr, &volume), "Create raster volume");
            vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            vd.Texture3D = {1, 1, UINT32_MAX};
            check(d.device->CreateRenderTargetView(volume.Get(), &vd, &view), "Create raster volume subset");
            d.context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
            const auto volumeState = pixelRasterState(d.context.Get(), 0);
            QCOMPARE(volumeState.target.width, 16u);
            QCOMPARE(volumeState.target.height, 8u);
            QCOMPARE(volumeState.target.layers, 3u);
            d.validate();
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
        const PixelInputRecordSelection selection{1, 1, 0, 0, 1, false};
        const auto records = pixelInputShader(original, reg, selection);
        QVERIFY(records.sampleRecordTarget);
        QVERIFY(std::none_of(records.inputSignature.begin(), records.inputSignature.end(),
                             [](const auto &field) { return field.system == 10; }));
        bool retainedTrigger = false;
        for (const auto &[tag, bytes] : readDxbcParts(records.bytecode))
            if (tag == 0x58454853)
                for (const auto &row : readDxbcProgram(bytes).instructions)
                    if ((row[0] & 2047) == 98 && ((row[0] >> 11) & 15) == 6)
                        retainedTrigger = true;
        QVERIFY(retainedTrigger);
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> owned;
        bool emptied = false;
        for (const auto &[tag, bytes] : readDxbcParts(original)) {
            std::vector<uint8_t> copy(bytes.begin(), bytes.end());
            if (tag == 0x58454853) {
                auto program = readDxbcProgram(bytes);
                for (auto &row : program.instructions)
                    if ((row[0] & 2047) == 98 && ((row[0] >> 11) & 15) == 6) {
                        row[1] &= ~0xf0u;
                        emptied = true;
                    }
                copy = writeDxbcProgram(program);
            }
            owned.emplace_back(tag, std::move(copy));
        }
        QVERIFY(emptied);
        DxbcParts parts;
        for (const auto &[tag, bytes] : owned)
            parts.emplace_back(tag, bytes);
        const auto malformed = makeDxbc(parts);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, pixelInputShader(malformed, reg, selection));
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
