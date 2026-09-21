#include "application/DxbcCoverage.h"
#include "application/DxbcSystemId.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QCoreApplication>
#include <QFile>
#include <QtTest>
using namespace flora;
using Json = nlohmann::json;
namespace {
uint32_t uintValue(const Json &value) {
    if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("Expected uint32");
    return value.get<uint32_t>();
}
Json process(const Json &job) {
    const auto input = QByteArray::fromBase64(QByteArray::fromStdString(job.at("input").get<std::string>()),
                                              QByteArray::AbortOnBase64DecodingErrors);
    if (input.isEmpty())
        throw std::runtime_error("Invalid shader input");
    Bytes raw(reinterpret_cast<const uint8_t *>(input.data()), input.size());
    std::vector<uint8_t> output;
    Json result;
    if (job.at("action") == "offset") {
        auto patched = offsetSystemId(raw, uintValue(job.at("offset")), uintValue(job.at("system")));
        output = std::move(patched.bytes);
        result["metadata"] = std::move(patched.metadata);
    } else if (job.at("action") == "filter") {
        std::optional<uint32_t> index;
        if (job.contains("index") && !job["index"].is_null())
            index = uintValue(job["index"]);
        output = filterQuadArrayIndex(raw, index, job.value("source", Json(nullptr)));
    } else
        throw std::runtime_error("Unknown Quad shader operation");
    result["output"] =
        QByteArray(reinterpret_cast<const char *>(output.data()), output.size()).toBase64().toStdString();
    return result;
}
std::vector<uint32_t> identities(bool warp, bool split) {
    const auto raw = compileHlsl("struct O{float4 p:SV_Position;uint2 ids:TEXCOORD;};"
                                 "O main(uint v:SV_VertexID,uint i:SV_InstanceID){O o;"
                                 "o.p=float4(0,0,0,1);o.ids=uint2(v,i);return o;}",
                                 "vs_5_0")
                         .bytecode;
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
          "Create identity test device");
    const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 0, 0, 2, 0};
    const UINT stride = 8;
    Com<ID3D11GeometryShader> gs;
    check(device->CreateGeometryShaderWithStreamOutput(raw.data(), raw.size(), &entry, 1, &stride, 1,
                                                       D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs),
          "Create identity stream capture");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 6 * 4 * 8;
    desc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    Com<ID3D11Buffer> buffer;
    check(device->CreateBuffer(&desc, nullptr, &buffer), "Create identity output");
    auto target = buffer.Get();
    const UINT zero = 0;
    context->SOSetTargets(1, &target, &zero);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    auto submit = [&](Bytes code, UINT count, UINT instances, UINT first) {
        Com<ID3D11VertexShader> vs;
        check(device->CreateVertexShader(code.data(), code.size(), nullptr, &vs), "Create compensated VS");
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->DrawInstanced(count, instances, first, 2);
    };
    if (!split)
        submit(raw, 6, 4, 7);
    else
        for (uint32_t instance = 0; instance < 4; ++instance) {
            const auto code = offsetSystemId(raw, instance, 8).bytes;
            for (uint32_t first = 0; first < 6; first += 3)
                submit(offsetSystemId(code, first, 6).bytes, 3, 1, 7 + first);
        }
    context->SOSetTargets(0, nullptr, nullptr);
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Buffer> staging;
    check(device->CreateBuffer(&desc, nullptr, &staging), "Create identity staging");
    context->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read identities");
    std::vector<uint32_t> result(desc.ByteWidth / 4);
    std::memcpy(result.data(), mapped.pData, desc.ByteWidth);
    context->Unmap(staging.Get(), 0);
    return result;
}
} // namespace
class DxbcQuadTests final : public QObject {
    Q_OBJECT
  private slots:
    void splitPreservesNativeIdentities() {
        for (bool warp : {false, true}) {
            const auto original = identities(warp, false);
            QCOMPARE(identities(warp, true), original);
            for (uint32_t instance = 0; instance < 4; ++instance)
                for (uint32_t vertex = 0; vertex < 6; ++vertex) {
                    QCOMPARE(original[(instance * 6 + vertex) * 2], vertex);
                    QCOMPARE(original[(instance * 6 + vertex) * 2 + 1], instance);
                }
        }
    }
    void noOpAndUnsupportedStage() {
        const auto vertex =
            compileHlsl("float4 main(uint id:SV_VertexID):SV_Position{return id;}", "vs_5_0").bytecode;
        QCOMPARE(offsetSystemId(vertex, 0, 6).bytes, vertex);
        QCOMPARE(offsetSystemId(vertex, 9, 8).bytes, vertex);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, offsetSystemId(vertex, 1, 7));
        const auto pixel = compileHlsl("void main(){}", "ps_5_0").bytecode;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, offsetSystemId(pixel, 0, 6));
    }
    void implicitArraySelection() {
        const auto pixel =
            compileHlsl("RWBuffer<uint> hits:register(u4);void main(){InterlockedAdd(hits[0],1);}", "ps_5_0")
                .bytecode;
        QCOMPARE(filterQuadArrayIndex(pixel, {}), pixel);
        QCOMPARE(filterQuadArrayIndex(pixel, 0), pixel);
        QCOMPARE(filterQuadArrayIndex(pixel, 1), compileHlsl("void main(){}", "ps_5_0").bytecode);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 4 && args[1] == "--probe") {
        QFile input(args[2]);
        if (!input.open(QIODevice::ReadOnly))
            return 2;
        Json results = Json::array();
        for (const auto &job : Json::parse(input.readAll().toStdString()))
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
    DxbcQuadTests test;
    return QTest::qExec(&test, argc, argv);
}
#include "DxbcQuadTests.moc"
