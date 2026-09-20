#include "application/DxbcIdentity.h"
#include "application/DxbcInspection.h"
#include "application/DxbcOutputLog.h"
#include "replay/Replay.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
#include <bit>
#include <chrono>
#include <d3dcompiler.h>
#include <thread>
using namespace flora;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> read(const std::string &path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read shader probe input");
    const auto bytes = f.readAll();
    return {bytes.begin(), bytes.end()};
}
void write(const std::string &path, Bytes bytes) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly) ||
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Cannot write shader probe output");
}
std::vector<uint8_t> compile(const std::string &source, const char *profile) {
    Com<ID3DBlob> binary, errors;
    auto hr = D3DCompile(source.data(), source.size(), "log-test", nullptr, nullptr, "main", profile,
                         D3DCOMPILE_ENABLE_STRICTNESS, 0, &binary, &errors);
    if (FAILED(hr))
        throw std::runtime_error(errors ? static_cast<const char *>(errors->GetBufferPointer())
                                        : "Compile failed");
    auto data = static_cast<const uint8_t *>(binary->GetBufferPointer());
    return {data, data + binary->GetBufferSize()};
}
const std::string vs =
    "float4 main(uint id:SV_VertexID,uint inst:SV_InstanceID):SV_Position{return float4(id,inst,0,1);}";
const std::string gs = "struct V{float4 p:SV_Position;};[maxvertexcount(2)]void main(point V input[1],uint "
                       "prim:SV_PrimitiveID,inout PointStream<V> s){V "
                       "v;v.p=input[0].p;v.p.x+=prim;s.Append(v);s.Append(v);s.RestartStrip();}";
int probe(const std::string &manifest) {
    const auto raw = read(manifest);
    const auto jobs = Json::parse(raw);
    Json results = Json::array();
    for (const auto &job : jobs) {
        Json result{{"name", job.at("name")}};
        try {
            const auto code = read(job.at("input"));
            OutputLogShader patched;
            const auto stage = job.at("stage").get<std::string>();
            if (stage == "identity") {
                auto identity = instrumentVertexIdentity(code, job.value("instances", 2u));
                patched = {std::move(identity.bytes), std::move(identity.markers)};
            } else if (stage == "gs")
                patched = instrumentGeometryEmissions(code, job.at("slot"), job.at("capacity"),
                                                      job.value("stream", 0u));
            else
                patched = instrumentOutputWrites(code, job.at("slot"), job.at("capacity"));
            result["metadata"] = patched.metadata;
            write(job.at("output"), patched.bytes);
            result["success"] = true;
        } catch (const std::exception &error) {
            result["success"] = false;
            result["error"] = error.what();
        }
        results.push_back(std::move(result));
    }
    const auto text = results.dump(2);
    write(manifest + ".results.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    return 0;
}
} // namespace
class DxbcLogTests final : public QObject {
    Q_OBJECT
  private slots:
    void nativeRecords() {
        const std::string common = "struct V{float4 p:SV_Position;};struct C{float "
                                   "edge[3]:SV_TessFactor;float inside:SV_InsideTessFactor;};";
        const auto hull =
            compile(common + "C patch(InputPatch<V,1> v){C c;c.edge[0]=c.edge[1]=c.edge[2]=c.inside=1;return "
                             "c;}[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"point\")]["
                             "outputcontrolpoints(1)][patchconstantfunc(\"patch\")]V main(InputPatch<V,1> "
                             "v,uint id:SV_OutputControlPointID){return v[0];}",
                    "hs_5_0");
        const auto domain = compile(
            common +
                "[domain(\"tri\")]float4 main(C c,float3 uv:SV_DomainLocation,uint prim:SV_PrimitiveID,const "
                "OutputPatch<V,1> v):SV_Position{return float4(uv.xy,prim,1);}",
            "ds_5_0");
        const auto vertex = compile(vs, "vs_5_0"), geometry = compile(gs, "gs_5_0");
        auto parts = readDxbcParts(geometry);
        auto code =
            std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == 0x58454853; });
        QVERIFY(code != parts.end());
        auto program = readDxbcProgram(code->second);
        unsigned emissions = 0, changed = 0;
        for (auto &row : program.instructions) {
            const auto opcode = row[0] & 2047;
            if (opcode == 117 || opcode == 19)
                ++emissions;
            else if (emissions == 1 && opcode == 54 && ((row[1] >> 12) & 255) == 2 &&
                     ((row[1] >> 4) & 15) == 14) {
                row[1] = (row[1] & ~240u) | 32;
                ++changed;
            }
        }
        QCOMPARE(changed, 1u);
        const auto sparseProgram = writeDxbcProgram(program);
        code->second = sparseProgram;
        const auto sparseGeometry = makeDxbc(parts);
        const auto pixel = compile("float4 main():SV_Target{return 1;}", "ps_5_0");
        for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            Com<ID3D11Device> device;
            Com<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_1};
            check(D3D11CreateDevice(nullptr, driver, nullptr, 0, requested, 1, D3D11_SDK_VERSION, &device,
                                    nullptr, &context),
                  "Create log execution device");
            Com<ID3D11Texture2D> target;
            D3D11_TEXTURE2D_DESC td{
                8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET,
                0, 0};
            check(device->CreateTexture2D(&td, nullptr, &target), "Create log witness target");
            Com<ID3D11RenderTargetView> rtv;
            check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create log witness RTV");
            Com<ID3D11PixelShader> ps;
            check(device->CreatePixelShader(pixel.data(), pixel.size(), nullptr, &ps),
                  "Create log witness PS");
            for (const std::string stage : {"vs", "ds", "gs", "gs-sparse"}) {
                const bool isGs = stage.starts_with("gs");
                for (auto capacity : {1u, 64u}) {
                    context->ClearState();
                    auto patched =
                        isGs ? instrumentGeometryEmissions(stage == "gs-sparse" ? sparseGeometry : geometry,
                                                           63, capacity)
                             : instrumentOutputWrites(stage == "ds" ? domain : vertex, 63, capacity);
                    const auto &vbytes = stage == "vs" ? patched.bytes : vertex;
                    Com<ID3D11VertexShader> v;
                    check(device->CreateVertexShader(vbytes.data(), vbytes.size(), nullptr, &v),
                          "Create logged VS");
                    context->VSSetShader(v.Get(), nullptr, 0);
                    Com<ID3D11HullShader> h;
                    Com<ID3D11DomainShader> d;
                    Com<ID3D11GeometryShader> g;
                    if (stage == "ds") {
                        check(device->CreateHullShader(hull.data(), hull.size(), nullptr, &h),
                              "Create log HS");
                        check(device->CreateDomainShader(patched.bytes.data(), patched.bytes.size(), nullptr,
                                                         &d),
                              "Create logged DS");
                        context->HSSetShader(h.Get(), nullptr, 0);
                        context->DSSetShader(d.Get(), nullptr, 0);
                    }
                    if (isGs) {
                        check(device->CreateGeometryShader(patched.bytes.data(), patched.bytes.size(),
                                                           nullptr, &g),
                              "Create logged GS");
                        context->GSSetShader(g.Get(), nullptr, 0);
                    }
                    context->PSSetShader(ps.Get(), nullptr, 0);
                    context->IASetPrimitiveTopology(stage == "ds"
                                                        ? D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST
                                                        : D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
                    D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
                    context->RSSetViewports(1, &viewport);
                    const auto size = patched.metadata.at("total_bytes").get<UINT>();
                    // An extra sentinel beyond the declared log verifies bounded stores on overflow.
                    std::vector<uint32_t> initial(size / 4 + 16, 0);
                    std::fill(initial.end() - 16, initial.end(), 0xdeadbeefu);
                    D3D11_BUFFER_DESC bd{size + 64,
                                         D3D11_USAGE_DEFAULT,
                                         D3D11_BIND_UNORDERED_ACCESS,
                                         0,
                                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                                         0};
                    D3D11_SUBRESOURCE_DATA init{initial.data(), 0, 0};
                    Com<ID3D11Buffer> buffer;
                    check(device->CreateBuffer(&bd, &init, &buffer), "Create log buffer");
                    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = DXGI_FORMAT_R32_TYPELESS;
                    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                    ud.Buffer.NumElements = UINT(initial.size());
                    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
                    Com<ID3D11UnorderedAccessView> uav;
                    check(device->CreateUnorderedAccessView(buffer.Get(), &ud, &uav), "Create log UAV");
                    auto view = uav.Get();
                    auto render = rtv.Get();
                    context->OMSetRenderTargetsAndUnorderedAccessViews(1, &render, nullptr, 63, 1, &view,
                                                                       nullptr);
                    Com<ID3D11Query> query;
                    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
                    check(device->CreateQuery(&qd, &query), "Create log pipeline witness");
                    context->Begin(query.Get());
                    context->DrawInstanced(stage == "ds" ? 2 : 7, 2, 0, 0);
                    context->End(query.Get());
                    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    for (;;) {
                        auto hr = context->GetData(query.Get(), &stats, sizeof stats, 0);
                        check(hr, "Read log pipeline witness");
                        if (hr == S_OK)
                            break;
                        if (std::chrono::steady_clock::now() > deadline)
                            QFAIL("Log witness timeout");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    context->ClearState();
                    bd.Usage = D3D11_USAGE_STAGING;
                    bd.BindFlags = 0;
                    bd.MiscFlags = 0;
                    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    Com<ID3D11Buffer> staging;
                    check(device->CreateBuffer(&bd, nullptr, &staging), "Create log readback");
                    context->CopyResource(staging.Get(), buffer.Get());
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map log records");
                    std::vector<uint32_t> words(initial.size());
                    std::memcpy(words.data(), mapped.pData, words.size() * 4);
                    context->Unmap(staging.Get(), 0);
                    QVERIFY(
                        std::all_of(words.end() - 16, words.end(), [](auto w) { return w == 0xdeadbeefu; }));
                    QCOMPARE(words[1], 0u);
                    QCOMPARE(words[3], 0u);
                    const auto invocations = stage == "vs"   ? stats.VSInvocations
                                             : stage == "ds" ? stats.DSInvocations
                                                             : stats.GSInvocations;
                    QVERIFY(invocations > 0);
                    if (isGs) {
                        QCOMPARE(uint64_t(words[2]), invocations);
                        QCOMPARE(uint64_t(words[0]),
                                 invocations * (patched.metadata.at("emit_sites").get<unsigned>() +
                                                patched.metadata.at("cut_sites").get<unsigned>()));
                    } else
                        QCOMPARE(uint64_t(words[0]), invocations);
                    const auto stride = patched.metadata.at("record_stride").get<unsigned>() / 4;
                    const auto regs = patched.metadata.at("registers").get<unsigned>();
                    std::map<uint32_t, std::set<uint32_t>> ordinals;
                    for (unsigned row = 0; row < std::min(words[0], capacity); ++row) {
                        const auto base = 4 + row * stride;
                        const auto values = base + (isGs ? 8 : 4);
                        const auto flags = values + regs * 4;
                        bool cut = false;
                        if (isGs) {
                            QVERIFY(words[base] < words[2]);
                            QVERIFY(ordinals[words[base]].insert(words[base + 1]).second);
                            QCOMPARE(words[base + 4], 0u);
                            QVERIFY(words[base + 5] >= 1 && words[base + 5] <= 3);
                            cut = words[base + 5] == 2;
                        }
                        const bool sparseRecord = stage == "gs-sparse" && words[base + 1] == 1;
                        for (unsigned c = 0; c < 4; ++c) {
                            const bool written = !cut && !(sparseRecord && c >= 2);
                            QCOMPARE(words[flags + c], written ? 1u : 0u);
                            if (!written)
                                QCOMPARE(words[values + c], 0u);
                        }
                        if (cut)
                            continue;
                        if (stage == "vs") {
                            QVERIFY(words[base] < 7);
                            QVERIFY(words[base + 1] < 2);
                            QCOMPARE(std::bit_cast<float>(words[values]), float(words[base]));
                            QCOMPARE(std::bit_cast<float>(words[values + 1]), float(words[base + 1]));
                        } else if (stage == "ds") {
                            QVERIFY(words[base] < 2);
                            QCOMPARE(words[values], words[base + 1]);
                            QCOMPARE(words[values + 1], words[base + 2]);
                            QCOMPARE(std::bit_cast<float>(words[values + 2]), float(words[base]));
                        } else {
                            QCOMPARE(std::bit_cast<float>(words[values]), float(words[base + 2] * 2));
                        }
                        QCOMPARE(std::bit_cast<float>(words[values + 3]), sparseRecord ? 0.0f : 1.0f);
                    }
                    if (isGs && capacity >= words[0]) {
                        QCOMPARE(ordinals.size(), size_t(invocations));
                        for (const auto &[id, sequence] : ordinals) {
                            unsigned expected = 0;
                            for (auto ordinal : sequence)
                                QCOMPARE(ordinal, expected++);
                        }
                    }
                }
            }
        }
    }
    void deviceAcceptance() {
        const auto vertex = compile(vs, "vs_5_0"), geometry = compile(gs, "gs_5_0");
        for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            Com<ID3D11Device> device;
            Com<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL actual{}, requested[]{D3D_FEATURE_LEVEL_11_1};
            check(D3D11CreateDevice(nullptr, driver, nullptr, 0, requested, 1, D3D11_SDK_VERSION, &device,
                                    &actual, &context),
                  "Create output log test device");
            for (auto slot : {0u, 7u, 8u, 63u}) {
                const auto a = instrumentOutputWrites(vertex, slot, 3);
                const auto b = instrumentGeometryEmissions(geometry, slot, 3);
                Com<ID3D11VertexShader> v;
                Com<ID3D11GeometryShader> g;
                check(device->CreateVertexShader(a.bytes.data(), a.bytes.size(), nullptr, &v),
                      "Create patched VS");
                check(device->CreateGeometryShader(b.bytes.data(), b.bytes.size(), nullptr, &g),
                      "Create patched GS");
                QCOMPARE(a.metadata.at("known_inputs").size(), size_t(2));
                QVERIFY(b.metadata.at("known_inputs").contains("primitive"));
            }
        }
    }
    void checkedRejections() {
        const auto vertex = compile(vs, "vs_5_0"), geometry = compile(gs, "gs_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentOutputWrites(vertex, 64, 1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentOutputWrites(vertex, 63, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentOutputWrites(vertex, 63, UINT32_MAX));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentGeometryEmissions(geometry, 63, 1, 1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentOutputWrites(geometry, 63, 1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentGeometryEmissions(vertex, 63, 1));
        const auto collision = compile("RWByteAddressBuffer b:register(u7);float4 main(uint "
                                       "i:SV_VertexID):SV_Position{b.Store(i*4,i);return float4(i,0,0,1);}",
                                       "vs_5_0");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, instrumentOutputWrites(collision, 7, 1));
        QVERIFY(!instrumentOutputWrites(collision, 63, 1).bytes.empty());
        for (size_t n : {size_t(0), size_t(3), size_t(32), vertex.size() - 1})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     instrumentOutputWrites(Bytes(vertex).first(n), 63, 1));
    }
    void operandBounds() {
        std::set<unsigned> unused;
        // Nested relative index whose literal resembles a UAV must remain an immediate.
        const std::vector<uint32_t> relative{0x00900002, 0x4001, 0x11e000};
        QCOMPARE(dxbc_detail::operand(relative, 0, unused, 0, false), relative.size());
        const std::vector<uint32_t> doubleVector{0x5002, 1, 2, 3, 4};
        QCOMPARE(dxbc_detail::operand(doubleVector, 0, unused, 0, false), doubleVector.size());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 dxbc_detail::operand({0x5001, 0, 0}, 0, unused, 0, false));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 dxbc_detail::operand({0x4002, 1, 2, 3}, 0, unused, 0, false));
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, dxbc_detail::operand({0x80100002}, 0, unused, 0, false));
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string(argv[1]) == "--probe") {
        try {
            return probe(argv[2]);
        } catch (const std::exception &e) {
            qCritical("%s", e.what());
            return 1;
        }
    }
    DxbcLogTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "DxbcLogTests.moc"
