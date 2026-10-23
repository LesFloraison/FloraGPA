#include "SyntheticCapture.h"
#include "application/NativePixel.h"
#include "replay/Replay.h"
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <thread>
using namespace flora;
using namespace flora::testing;
using Json = nlohmann::json;
namespace {
std::vector<uint8_t> storage(uint32_t format) {
    std::vector<uint8_t> bytes(pitches(1, 1, format).first);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = uint8_t(0x80 + 37 * i);
    if (format == 2 || format == 6 || format == 16 || format == 41) {
        const float values[]{1.5f, -2.25f, 0.f, 100.f};
        std::memcpy(bytes.data(), values, bytes.size());
    }
    if (format == 10 || format == 34 || format == 54) {
        const uint16_t values[]{0x3c00, 0xbc00, 0x0001, 0x03ff};
        std::memcpy(bytes.data(), values, bytes.size());
    }
    return bytes;
}
std::array<uint32_t, 4> gpuLoad(Replay &replay, uint32_t resourceFormat, uint32_t viewFormat, Bytes bytes,
                                const std::string &numeric, uint32_t x = 0, uint32_t y = 0) {
    auto *device = replay.nativeDevice();
    const bool block =
        (resourceFormat >= 70 && resourceFormat <= 84) || (resourceFormat >= 94 && resourceFormat <= 99);
    const auto extent = block ? 4u : 1u;
    D3D11_TEXTURE2D_DESC textureDesc{extent,
                                     extent,
                                     1,
                                     1,
                                     DXGI_FORMAT(resourceFormat),
                                     {1, 0},
                                     D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_SHADER_RESOURCE,
                                     0,
                                     0};
    const D3D11_SUBRESOURCE_DATA initial{bytes.data(), UINT(bytes.size()), UINT(bytes.size())};
    Com<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&textureDesc, &initial, &texture),
          "Create pixel decoding reference texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = DXGI_FORMAT(viewFormat);
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    Com<ID3D11ShaderResourceView> view;
    check(device->CreateShaderResourceView(texture.Get(), &viewDesc, &view),
          "Create pixel decoding reference view");
    D3D11_BUFFER_DESC bufferDesc{
        16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
    Com<ID3D11Buffer> output;
    check(device->CreateBuffer(&bufferDesc, nullptr, &output), "Create pixel decoding reference output");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 1;
    Com<ID3D11UnorderedAccessView> uav;
    check(device->CreateUnorderedAccessView(output.Get(), &uavDesc, &uav), "Create reference UAV");
    const std::string type = numeric == "sint" ? "int" : numeric;
    const auto source = "Texture2D<" + type +
                        "4> inputTexture:register(t0);"
                        "RWStructuredBuffer<uint4> outputValues:register(u0);[numthreads(1,1,1)]"
                        "void main(){outputValues[0]=asuint(inputTexture.Load(int3(" +
                        std::to_string(x) + "," + std::to_string(y) + ",0)));}";
    Com<ID3DBlob> code, errors;
    const auto compiled = D3DCompile(source.data(), source.size(), "Native pixel oracle", nullptr, nullptr,
                                     "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(compiled) && errors)
        throw std::runtime_error(
            std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()));
    check(compiled, "Compile pixel decoding reference");
    Com<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader),
          "Create pixel decoding reference shader");
    Com<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    auto *srv = view.Get();
    auto *target = uav.Get();
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch(1, 1, 1);
    context->ClearState();
    bufferDesc.Usage = D3D11_USAGE_STAGING;
    bufferDesc.BindFlags = bufferDesc.MiscFlags = bufferDesc.StructureByteStride = 0;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Buffer> staging;
    check(device->CreateBuffer(&bufferDesc, nullptr, &staging), "Create reference staging");
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read reference pixel");
    std::array<uint32_t, 4> result;
    std::memcpy(result.data(), mapped.pData, sizeof result);
    context->Unmap(staging.Get(), 0);
    Com<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)), "Get reference validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Get reference message size");
        std::vector<uint8_t> data(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
        check(queue->GetMessage(i, message, &size), "Get reference message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    return result;
}
ReplayOptions deviceOptions(uint32_t vendor) {
    ReplayOptions result;
    result.debug = true;
    result.warp = !vendor;
    if (vendor)
        result.vendor = vendor;
    return result;
}
void frameFile(const QString &path) {
    Capture c;
    c.add(1, 5, 0x127, std::vector<uint8_t>(24));
    c.save(path);
}
} // namespace
class NativePixelTests final : public QObject {
    Q_OBJECT
  private slots:
    void colorHardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format :
                 {2u,  3u,  4u,  6u,  7u,  8u,  10u, 11u, 12u, 13u, 14u, 16u, 17u, 18u, 24u, 25u, 26u, 28u,
                  29u, 30u, 31u, 32u, 34u, 35u, 36u, 37u, 38u, 41u, 42u, 43u, 49u, 50u, 51u, 52u, 54u, 56u,
                  57u, 58u, 59u, 61u, 62u, 63u, 64u, 65u, 67u, 85u, 86u, 87u, 88u, 91u, 93u, 115u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(format))) << vendor << format;
    }
    void colorHardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            frameFile(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, deviceOptions(vendor));
            const auto bytes = storage(format);
            NativePixelDecoder decoder(replay);
            const auto decoded = decoder.decode(bytes, format, 0, 0);
            QCOMPARE(decoded.at("status"), Json("decoded"));
            const auto &color = decoded.at("color");
            const auto numeric = color.at("numeric_type").get<std::string>();
            const auto actual = gpuLoad(replay, format, format, bytes, numeric);
            if (format == 85 || format == 86) {
                QCOMPARE(decoded.at("source"), Json("native_gpu_load"));
                QCOMPARE(decoded.at("storage_math_color"), decodeNativePixel(bytes, format).at("color"));
                QCOMPARE(decoded.at("color").at("component_bits"), Json(actual));
                qInfo().noquote() << QString::fromStdString(Json{
                    {"vendor", vendor},
                    {"format", format},
                    {"storage_math", decoded.at("storage_math_color").at("rgba")},
                    {"gpu_load", decoded.at("color").at("rgba")},
                    {"gpu_bits", actual}}.dump());
            }
            for (size_t i = 0; i < 4; ++i) {
                const auto &expected = color.at("rgba")[i];
                if (numeric == "uint")
                    QCOMPARE(actual[i], expected.get<uint32_t>());
                else if (numeric == "sint")
                    QCOMPARE(std::bit_cast<int32_t>(actual[i]), expected.get<int32_t>());
                else {
                    const double value = std::bit_cast<float>(actual[i]);
                    const double reference = expected.get<double>();
                    if (color.at("srgb_to_linear") == true && i < 3) {
                        const auto encode = [](double linear) {
                            return (linear <= 0.0031308 ? 12.92 * linear
                                                        : 1.055 * std::pow(linear, 1. / 2.4) - 0.055) *
                                   255.;
                        };
                        QVERIFY2(std::abs(encode(value) - encode(reference)) <= 0.5,
                                 "sRGB conversion exceeds the specified half-code tolerance");
                    } else if (format == 2 || format == 6 || format == 10 || format == 16 || format == 26 ||
                               format == 34 || format == 41 || format == 54 || format == 67)
                        QCOMPARE(value, reference);
                    else
                        QVERIFY2(std::abs(value - reference) <=
                                     2. * FLT_EPSILON * std::max(1., std::abs(reference)),
                                 ("GPU Load differs from storage decoding: " + std::to_string(value) +
                                  " vs " + expected.dump())
                                     .c_str());
                }
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void depthHardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void depthHardware() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            frameFile(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, deviceOptions(vendor));
            for (uint32_t format : {20u, 40u, 45u, 55u}) {
                auto bytes = storage(format);
                if (format == 20 || format == 40) {
                    const float value = .375f;
                    std::memcpy(bytes.data(), &value, 4);
                }
                const uint32_t resourceFormat = format == 20   ? 19
                                                : format == 40 ? 39
                                                : format == 45 ? 44
                                                               : 53;
                const uint32_t viewFormat = format == 20 ? 21 : format == 40 ? 41 : format == 45 ? 46 : 56;
                const auto decoded = decodeNativePixel(bytes, format);
                const auto depth = gpuLoad(replay, resourceFormat, viewFormat, bytes, "float");
                QVERIFY(std::abs(double(std::bit_cast<float>(depth[0])) -
                                 decoded.at("depth").get<double>()) <= FLT_EPSILON);
                if (format == 20 || format == 45) {
                    const auto stencil =
                        gpuLoad(replay, resourceFormat, format == 20 ? 22 : 47, bytes, "uint");
                    QCOMPARE(stencil[1], decoded.at("stencil").get<uint32_t>());
                }
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void exactAndUnknownValues() {
        QCOMPARE(decodeNativePixel(std::array<uint8_t, 4>{0, 0, 0, 0x80}, 41).at("color").at("rgba")[0],
                 Json(-0.));
        QVERIFY(std::signbit(decodeNativePixel(std::array<uint8_t, 4>{0, 0, 0, 0x80}, 41)
                                 .at("color")
                                 .at("rgba")[0]
                                 .get<double>()));
        QCOMPARE(
            decodeNativePixel(std::array<uint8_t, 4>{0x34, 0x12, 0xc0, 0x7f}, 41).at("color").at("rgba")[0],
            Json("nan"));
        QCOMPARE(decodeNativePixel(std::array<uint8_t, 4>{0, 0, 0x80, 0xff}, 41).at("color").at("rgba")[0],
                 Json("-inf"));
        QCOMPARE(decodeNativePixel(std::array<uint8_t, 1>{0x80}, 63).at("color").at("rgba")[0], Json(-1.));
        QCOMPARE(
            decodeNativePixel(std::array<uint8_t, 4>{0xff, 0xff, 0xff, 0xff}, 42).at("color").at("rgba")[0],
            Json(UINT32_MAX));
        QCOMPARE(decodeNativePixel(std::array<uint8_t, 4>{}, 27).at("status"), Json("unknown"));
        QCOMPARE(decodeNativePixel(std::array<uint8_t, 8>{}, 71).at("status"), Json("unsupported"));
        QVERIFY(nativePixelFormatsCompatible(27, 29));
        QVERIFY(!nativePixelFormatsCompatible(27, 41));
        QVERIFY(!nativePixelFormatsCompatible(28, 27));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, decodeNativePixel(std::array<uint8_t, 3>{}, 28));
    }
    void compressedHardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format : {71u, 72u, 74u, 75u, 77u, 78u, 80u, 81u, 83u, 84u, 95u, 96u, 98u, 99u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(format))) << vendor << format;
    }
    void compressedHardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        try {
            std::vector<uint8_t> bytes(pitches(4, 4, format).first);
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[i] = uint8_t(i * 37 + 19);
            const std::array<uint8_t, 8> color{0, 0xf8, 0xe0, 7, 0xe4, 0xe4, 0xe4, 0xe4};
            if (format == 71 || format == 72)
                std::copy(color.begin(), color.end(), bytes.begin());
            if (format >= 74 && format <= 78)
                std::copy(color.begin(), color.end(), bytes.begin() + 8);
            if (format == 95 || format == 96)
                bytes[0] &= 0xfc; // Valid BC6H mode 00; remaining bits encode endpoints and indices.
            if (format == 98 || format == 99)
                bytes[0] = 0x40; // Valid BC7 mode 6 with nonzero endpoint/index payload.
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            frameFile(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, deviceOptions(vendor));
            NativePixelDecoder decoder(replay);
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x) {
                    const auto decoded = decoder.decode(bytes, format, x + 12, y + 8);
                    QCOMPARE(decoded.at("source"), Json("native_gpu_load"));
                    const auto actual = gpuLoad(replay, format, format, bytes, "float", x, y);
                    QCOMPARE(decoded.at("color").at("component_bits"), Json(actual));
                    if (format == 71 && x < 2)
                        QCOMPARE(decoded.at("color").at("rgba"),
                                 x == 0 ? Json::array({1., 0., 0., 1.}) : Json::array({0., 1., 0., 1.}));
                }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void isolatedQueriesAndBindings_data() { depthHardware_data(); }
    void isolatedQueriesAndBindings() {
        QFETCH(uint32_t, vendor);
        try {
            QTemporaryDir directory;
            const auto path = directory.filePath("frame.gpa_frame");
            frameFile(path);
            Frame frame(path.toStdWString());
            Replay replay(frame, deviceOptions(vendor));
            auto *device = replay.nativeDevice();
            Com<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            D3D11_BUFFER_DESC d{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            Com<ID3D11Buffer> constant;
            check(device->CreateBuffer(&d, nullptr, &constant), "Create retained constant buffer");
            auto *cb = constant.Get();
            context->CSSetConstantBuffers(0, 1, &cb);
            d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            d.StructureByteStride = 16;
            Com<ID3D11Buffer> counted;
            check(device->CreateBuffer(&d, nullptr, &counted), "Create retained counter buffer");
            D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
            vd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            vd.Buffer.NumElements = 1;
            vd.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_COUNTER;
            Com<ID3D11UnorderedAccessView> uav;
            check(device->CreateUnorderedAccessView(counted.Get(), &vd, &uav), "Create retained counter UAV");
            auto *target = uav.Get();
            const UINT initial = 37;
            context->CSSetUnorderedAccessViews(0, 1, &target, &initial);
            D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
            Com<ID3D11Predicate> predicate;
            check(device->CreatePredicate(&predicateDesc, &predicate), "Create retained predicate");
            context->Begin(predicate.Get());
            context->End(predicate.Get());
            context->SetPredication(predicate.Get(), TRUE);
            D3D11_QUERY_DESC queryDesc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            Com<ID3D11Query> query;
            check(device->CreateQuery(&queryDesc, &query), "Create isolated decoder statistics query");
            context->Begin(query.Get());
            NativePixelDecoder decoder(replay);
            const auto value = decoder.decode(storage(85), 85, 0, 0);
            QCOMPARE(value.at("source"), Json("native_gpu_load"));
            context->End(query.Get());
            Com<ID3D11Buffer> observed;
            context->CSGetConstantBuffers(0, 1, &observed);
            QCOMPARE(observed.Get(), constant.Get());
            Com<ID3D11UnorderedAccessView> observedUav;
            context->CSGetUnorderedAccessViews(0, 1, &observedUav);
            QCOMPARE(observedUav.Get(), uav.Get());
            Com<ID3D11Predicate> observedPredicate;
            BOOL comparison = FALSE;
            context->GetPredication(&observedPredicate, &comparison);
            QCOMPARE(observedPredicate.Get(), predicate.Get());
            QCOMPARE(comparison, TRUE);
            context->SetPredication(nullptr, FALSE);
            D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics{};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            HRESULT ready;
            while ((ready = context->GetData(query.Get(), &statistics, sizeof statistics, 0)) == S_FALSE) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Isolated decoder query timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(ready, "Read isolated decoder query");
            QCOMPARE(statistics.CSInvocations, UINT64(0));
            QCOMPARE(statistics.PSInvocations, UINT64(0));
            d = {16, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
            Com<ID3D11Buffer> counter;
            check(device->CreateBuffer(&d, nullptr, &counter), "Create counter inspection");
            context->CopyStructureCount(counter.Get(), 0, uav.Get());
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            Com<ID3D11Buffer> staging;
            check(device->CreateBuffer(&d, nullptr, &staging), "Create counter readback");
            context->CopyResource(staging.Get(), counter.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map retained counter");
            uint32_t valueAfter{};
            std::memcpy(&valueAfter, mapped.pData, 4);
            context->Unmap(staging.Get(), 0);
            QCOMPARE(valueAfter, initial);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(NativePixelTests)
#include "NativePixelTests.moc"
