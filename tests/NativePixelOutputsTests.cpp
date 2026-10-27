#include "application/DxbcGpuCompletion.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "application/NativePixelOutputs.h"
#include "core/Dxbc.h"
#include "replay/Device.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
#include <QtTest>
#include <bit>
#include <d3d11sdklayers.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
struct Oracle {
    std::array<std::array<uint32_t, 4>, 4> color{}, integer{};
    std::array<uint32_t, 4> depth{};
};
std::string source(const std::string &mode) {
    const auto depth = mode == "greater" ? "SV_DepthGreaterEqual"
                       : mode == "less"  ? "SV_DepthLessEqual"
                                         : "SV_Depth";
    return "struct O{float4 color:SV_Target0;" + std::string(mode == "signed" ? "int4" : "uint4") +
           " bits:SV_Target1;float depth:" + std::string(depth) +
           ";uint coverage:SV_Coverage;};O main(noperspective centroid float4 p:SV_Position){O o;"
           "float gradient=ddx_fine(p.x);" +
           (mode == "discard" ? std::string("if(p.x<1)discard;") : std::string()) +
           "o.color=float4(p.xy,gradient*.25,1);o.bits=" +
           (mode == "signed" ? std::string("int4(-1,-2147483647,asint(p.z),2147483647);")
                             : std::string("uint4(0x12345678,0x80000001,asuint(p.z),0xffffffff);")) +
           "o.depth=" + (mode == "less" ? std::string(".25") : std::string(".75")) +
           ";o.coverage=" + (mode == "coverage" ? std::string("p.y<1?0:1") : std::string("1")) +
           ";return o;}";
}
void validate(ID3D11Device *d) {
    Com<ID3D11InfoQueue> q;
    check(d->QueryInterface(IID_PPV_ARGS(&q)), "Query output validation");
    for (UINT64 i = 0; i < q->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(q->GetMessage(i, nullptr, &size), "Size output validation");
        std::vector<uint8_t> b(size);
        auto *m = reinterpret_cast<D3D11_MESSAGE *>(b.data());
        check(q->GetMessage(i, m, &size), "Read output validation");
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(m->pDescription);
    }
}
Oracle render(Dx11Device &device, Bytes ps, uint32_t mask, bool signedOutput) {
    auto *d = device.device.Get();
    auto *c = device.context.Get();
    c->ClearState();
    const auto vs =
        compileHlsl("float4 main(uint v:SV_VertexID):SV_Position{return float4(v==2?3:-1,v==1?3:-1,.5,1);}",
                    "vs_5_0")
            .bytecode;
    Com<ID3D11VertexShader> vertex;
    Com<ID3D11PixelShader> pixel;
    check(d->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex), "Create output oracle VS");
    check(d->CreatePixelShader(ps.data(), ps.size(), nullptr, &pixel), "Create output oracle PS");
    std::array<Com<ID3D11Texture2D>, 3> textures, readbacks;
    std::array<Com<ID3D11RenderTargetView>, 2> targets;
    Com<ID3D11DepthStencilView> depth;
    for (unsigned i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC td{
            2,
            2,
            1,
            1,
            i == 0   ? DXGI_FORMAT_R32G32B32A32_FLOAT
            : i == 1 ? (signedOutput ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_UINT)
                     : DXGI_FORMAT_D32_FLOAT,
            {1, 0},
            D3D11_USAGE_DEFAULT,
            UINT(i == 2 ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET),
            0,
            0};
        check(d->CreateTexture2D(&td, nullptr, &textures[i]), "Create output oracle texture");
        if (i == 2) {
            check(d->CreateDepthStencilView(textures[i].Get(), nullptr, &depth),
                  "Create output oracle depth view");
            c->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH, .5f, 0);
        } else {
            check(d->CreateRenderTargetView(textures[i].Get(), nullptr, &targets[i]),
                  "Create output oracle color view");
            const float zero[4]{};
            c->ClearRenderTargetView(targets[i].Get(), zero);
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(d->CreateTexture2D(&td, nullptr, &readbacks[i]), "Create output oracle readback");
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    Com<ID3D11RasterizerState> raster;
    check(d->CreateRasterizerState(&rd, &raster), "Create output oracle rasterizer");
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                  D3D11_COMPARISON_ALWAYS};
    Com<ID3D11DepthStencilState> ds;
    check(d->CreateDepthStencilState(&dd, &ds), "Create output oracle depth state");
    const D3D11_VIEWPORT vp{0, 0, 2, 2, 0, 1};
    const D3D11_RECT scissor{0, 0,
                             mask == 0                ? 0
                             : mask == 1 || mask == 5 ? 1
                                                      : 2,
                             mask == 0                ? 0
                             : mask == 1 || mask == 3 ? 1
                                                      : 2};
    c->RSSetState(raster.Get());
    c->RSSetViewports(1, &vp);
    c->RSSetScissorRects(1, &scissor);
    ID3D11RenderTargetView *views[]{targets[0].Get(), targets[1].Get()};
    c->OMSetRenderTargets(2, views, depth.Get());
    c->OMSetDepthStencilState(ds.Get(), 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex.Get(), nullptr, 0);
    c->PSSetShader(pixel.Get(), nullptr, 0);
    c->Draw(3, 0);
    c->ClearState();
    for (unsigned i = 0; i < 3; ++i)
        c->CopyResource(readbacks[i].Get(), textures[i].Get());
    GpuCompletion completion(d, c);
    completion.submit();
    Oracle result;
    for (unsigned i = 0; i < 3; ++i) {
        const auto mapped = completion.map(readbacks[i].Get());
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 2; ++x) {
                const auto *p =
                    static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * (i == 2 ? 4 : 16);
                if (i == 2)
                    std::memcpy(&result.depth[y * 2 + x], p, 4);
                else
                    std::memcpy((i == 0 ? result.color : result.integer)[y * 2 + x].data(), p, 16);
            }
        c->Unmap(readbacks[i].Get(), 0);
    }
    validate(d);
    return result;
}
PixelTraceOptions inputs(Bytes code, uint32_t mask) {
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "Reflect output fixture input");
    D3D11_SIGNATURE_PARAMETER_DESC p{};
    check(reflection->GetInputParameterDesc(0, &p), "Get output fixture input");
    PixelTraceOptions result;
    result.activeMask = uint8_t(mask);
    result.inputProvenance = {{"source", "owned-2x2-output-oracle"}};
    for (unsigned i = 0; i < 4; ++i)
        result.registers[i][{1, {p.Register}}] = Value::known(
            {std::bit_cast<uint32_t>(float(i % 2) + .5f), std::bit_cast<uint32_t>(float(i / 2) + .5f),
             std::bit_cast<uint32_t>(.5f), std::bit_cast<uint32_t>(1.f)});
    return result;
}
const Json &output(const Json &lane, const std::string &kind, unsigned index = 0) {
    for (const auto &o : lane.at("outputs"))
        if (o.at("kind") == kind && o.at("index") == index)
            return o;
    throw std::runtime_error("Missing shader output");
}
void evidence(const Json &r, Bytes bytecode) {
    const auto root = qEnvironmentVariable("FLORA_PIXEL_OUTPUT_EVIDENCE");
    if (root.isEmpty())
        return;
    QDir dir(root);
    if (!dir.exists() && !QDir().mkpath(root))
        throw std::runtime_error("Cannot create shader output evidence");
    QFile f(dir.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".json"));
    const auto text = r.dump(2);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        f.write(text.data(), qint64(text.size())) != qint64(text.size()))
        throw std::runtime_error("Cannot save shader output evidence");
    QFile shader(dir.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".dxbc"));
    if (!shader.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        shader.write(reinterpret_cast<const char *>(bytecode.data()), qint64(bytecode.size())) !=
            qint64(bytecode.size()))
        throw std::runtime_error("Cannot save shader output bytecode");
}
} // namespace
class NativePixelOutputsTests final : public QObject {
    Q_OBJECT
  private slots:
    void gpu_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("mask");
        QTest::addColumn<QString>("mode");
        for (uint32_t v : {0u, 4318u, 32902u})
            for (uint32_t m : {0u, 1u, 3u, 5u, 15u})
                for (const auto &s : {QString("ordinary"), QString("discard"), QString("coverage"),
                                      QString("greater"), QString("less"), QString("signed")})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(v).arg(m).arg(s))) << v << m << s;
    }
    void gpu() {
        try {
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, mask);
            QFETCH(QString, mode);
            auto device = createDx11Device(!vendor, true, vendor ? std::optional(vendor) : std::nullopt);
            InputSnapshot snapshot;
            snapshot.event = 7;
            snapshot.shader = 9;
            snapshot.stage = 4;
            snapshot.bytecode = compileHlsl(source(mode.toStdString()), "ps_5_0").bytecode;
            Com<IDXGIDevice> dxgi;
            Com<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC identity{};
            check(device.device.As(&dxgi), "Query output adapter device");
            check(dxgi->GetAdapter(&adapter), "Query output adapter");
            check(adapter->GetDesc(&identity), "Query output adapter identity");
            snapshot.adapterVendor = identity.VendorId;
            snapshot.adapterDevice = identity.DeviceId;
            snapshot.adapterLuid =
                (uint64_t(uint32_t(identity.AdapterLuid.HighPart)) << 32) | identity.AdapterLuid.LowPart;
            snapshot.inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            const auto selection = inputs(snapshot.bytecode, mask);
            auto result = evaluateNativePixelOutputs(snapshot, selection);
            const auto gpu = render(device, snapshot.bytecode, mask, mode == "signed");
            for (unsigned lane = 0; lane < 4; ++lane) {
                const auto &l = result.at("lanes")[lane];
                const bool discard = mode == "discard" && lane % 2 == 0;
                const bool coverage = mode != "coverage" || lane >= 2;
                QCOMPARE(l.at("discarded"), Json(discard));
                QCOMPARE(l.at("shader_active"), Json(bool(mask & (1u << lane)) && !discard));
                const auto &color = output(l, "color", 0), &integer = output(l, "color", 1),
                           &depth = output(l, mode == "greater" ? "depth_greater_equal"
                                              : mode == "less"  ? "depth_less_equal"
                                                                : "depth"),
                           &covered = output(l, "coverage");
                QCOMPARE(color.at("valid_mask"), Json(15));
                QCOMPARE(integer.at("valid_mask"), Json(15));
                QCOMPARE(integer.at("numeric"), Json(mode == "signed" ? "int32" : "uint32"));
                QCOMPARE(depth.at("valid_mask"), Json(1));
                QCOMPARE(covered.at("valid_mask"), Json(1));
                QCOMPARE(covered.at("raw_bits")[0], Json(coverage ? 1 : 0));
                if ((mask & (1u << lane)) && !discard && coverage) {
                    QCOMPARE(color.at("raw_bits"), Json(gpu.color[lane]));
                    QCOMPARE(integer.at("raw_bits"), Json(gpu.integer[lane]));
                    QCOMPARE(depth.at("raw_bits")[0], Json(gpu.depth[lane]));
                } else {
                    QCOMPARE(gpu.color[lane], (std::array<uint32_t, 4>{}));
                    QCOMPARE(gpu.integer[lane], (std::array<uint32_t, 4>{}));
                    QCOMPARE(gpu.depth[lane], std::bit_cast<uint32_t>(.5f));
                }
            }
            result["reference"] = {{"color", gpu.color},
                                   {"integer", gpu.integer},
                                   {"depth", gpu.depth},
                                   {"device", device.information()}};
            evidence(result, snapshot.bytecode);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void privateStorageAndFailures() {
        InputSnapshot s;
        s.event = 1;
        s.shader = 2;
        s.stage = 4;
        s.bytecode = compileHlsl("RWByteAddressBuffer b:register(u1);uint4 main():SV_Target{uint "
                                 "old;b.InterlockedAdd(0,1,old);return uint4(old,b.Load(4),9,11);}",
                                 "ps_5_0")
                         .bytecode;
        auto buffer = Buffer::known(std::vector<uint8_t>(16));
        uint32_t word = 17;
        std::memcpy(buffer.bytes.data() + 4, &word, 4);
        s.inputs.memory =
            std::make_shared<Memory>(std::map<uint64_t, Buffer>{{3, buffer}},
                                     std::map<Register, BufferView>{{{30, {1}}, {3, 0, 16, 0, {}, 0}}});
        PixelTraceOptions o;
        o.activeMask = 1;
        const auto a = evaluateNativePixelOutputs(s, o);
        QCOMPARE(s.inputs.memory->buffers().at(3).bytes, buffer.bytes);
        QCOMPARE(evaluateNativePixelOutputs(s, o).at("lanes"), a.at("lanes"));
        QCOMPARE(output(a.at("lanes")[0], "color").at("raw_bits"), Json::array({0, 17, 9, 11}));
        QCOMPARE(output(a.at("lanes")[1], "color").at("valid_mask"), Json(12));
        QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, o, [] { return true; }), OperationCancelled);
        o.maxBytes = 1;
        QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, o), std::runtime_error);
        o.maxBytes = 256 * 1024 * 1024;
        o.maxSteps = 1;
        QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, o), std::runtime_error);
        QCOMPARE(s.inputs.memory->buffers().at(3).bytes, buffer.bytes);
    }
    void partialAndMissingInputs() {
        InputSnapshot s;
        s.event = 1;
        s.shader = 2;
        s.stage = 4;
        s.bytecode = compileHlsl("float2 main(float4 p:TEXCOORD):SV_Target3{return float2(p.x,7);}", "ps_5_0")
                         .bytecode;
        PixelTraceOptions o;
        auto result = evaluateNativePixelOutputs(s, o);
        const auto &v = output(result.at("lanes")[0], "color", 3);
        QCOMPARE(v.at("declared_mask"), Json(3));
        QCOMPARE(v.at("valid_mask"), Json(2));
        QCOMPARE(v.at("raw_bits")[1], Json(std::bit_cast<uint32_t>(7.f)));
        o.registers[0][{1, {0}}] = Value::known({std::bit_cast<uint32_t>(-3.f), 0, 0, 0});
        result = evaluateNativePixelOutputs(s, o);
        QCOMPARE(output(result.at("lanes")[0], "color", 3).at("valid_mask"), Json(3));
        QCOMPARE(output(result.at("lanes")[0], "color", 3).at("raw_bits")[0],
                 Json(std::bit_cast<uint32_t>(-3.f)));
        QCOMPARE(output(result.at("lanes")[1], "color", 3).at("valid_mask"), Json(2));
    }
    void signatureBoundaries() {
        InputSnapshot s;
        s.event = 1;
        s.shader = 2;
        s.stage = 4;
        const auto original =
            compileHlsl("float4 main():SV_Target{return float4(1,2,3,4);}", "ps_5_0").bytecode;
        const auto parts = readDxbcParts(original);
        auto signature =
            std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == 0x4e47534f; });
        QVERIFY(signature != parts.end());
        const std::vector<uint8_t> bytes(signature->second.begin(), signature->second.end());
        const auto replace = [&](const std::vector<uint8_t> &data, uint32_t tag = 0x4e47534f) {
            auto updated = parts;
            updated[size_t(signature - parts.begin())] = {tag, data};
            s.bytecode = makeDxbc(updated);
        };
        const auto set = [](auto &data, size_t at, uint32_t value) {
            std::memcpy(data.data() + at, &value, 4);
        };
        for (size_t size : {size_t(0), size_t(3), size_t(7), size_t(31)}) {
            replace(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size));
            QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, {}), std::runtime_error);
        }
        for (const auto &[at, word] : std::vector<std::pair<size_t, uint32_t>>{
                 {0, 129}, {8, 0}, {16, 65}, {20, 0}, {24, 8}, {28, 0}}) {
            auto data = bytes;
            set(data, at, word);
            replace(data);
            QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, {}), std::runtime_error);
        }
        auto unterminated = bytes;
        std::fill(unterminated.begin() + 32, unterminated.end(), 'X');
        replace(unterminated);
        QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, {}), std::runtime_error);
        // Construct equivalent extended signatures to exercise their distinct layouts.
        for (bool precision : {false, true}) {
            std::vector<uint8_t> data(bytes.begin(), bytes.begin() + 8);
            data.insert(data.end(), 4, 0);
            data.insert(data.end(), bytes.begin() + 8, bytes.begin() + 32);
            if (precision)
                data.insert(data.end(), 4, 0);
            data.insert(data.end(), bytes.begin() + 32, bytes.end());
            uint32_t name = 0;
            std::memcpy(&name, bytes.data() + 8, 4);
            set(data, 12, name + (precision ? 8 : 4));
            replace(data, precision ? 0x3147534f : 0x3547534f);
            const auto r = evaluateNativePixelOutputs(s, {});
            QCOMPARE(output(r.at("lanes")[0], "color").at("valid_mask"), Json(15));
            set(data, 8, 1);
            replace(data, precision ? 0x3147534f : 0x3547534f);
            QVERIFY_EXCEPTION_THROWN(evaluateNativePixelOutputs(s, {}), std::runtime_error);
        }
    }
};
QTEST_GUILESS_MAIN(NativePixelOutputsTests)
#include "NativePixelOutputsTests.moc"
