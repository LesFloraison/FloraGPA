#include "application/DxbcCoverage.h"
#include "application/DxbcInspection.h"
#include "application/HlslCompilation.h"
#include "replay/Replay.h"
#include <QDir>
#include <QFile>
#include <QtTest>
#include <chrono>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <thread>

using namespace flora;
using Json = nlohmann::json;
namespace {
struct Fixture {
    std::vector<uint8_t> bytes;
    uint32_t opcode;
};
Fixture fixture(uint32_t mode, bool srv, bool pixel = false, uint32_t inputSlot = 0) {
    const std::array<std::string, 3> types{"Buffer<uint4>", "ByteAddressBuffer", "StructuredBuffer<uint4>"};
    const auto source = std::string(srv ? "" : "RW") + types.at(mode) + " inputData:register(" +
                        (srv ? "t0" : "u" + std::to_string(inputSlot)) +
                        ");RWStructuredBuffer<uint4> outputData:register(u1);" +
                        (pixel ? "void main(float4 p:SV_Position){" : "[numthreads(1,1,1)]void main(){") +
                        "uint status;uint4 value=inputData.Load(" + (mode == 1 ? "8" : "2") +
                        ",status);outputData[0]=value;outputData[1]=uint4("
                        "CheckAccessFullyMapped(status),99u,0x0011e000u,123u);}";
    return {compileHlsl(source, pixel ? "ps_5_0" : "cs_5_0").bytecode, mode == 0   ? (srv ? 223u : 225u)
                                                                       : mode == 1 ? 226u
                                                                                   : 227u};
}
DxbcProgram code(Bytes bytes) {
    for (const auto &[tag, data] : readDxbcParts(bytes))
        if (tag == 0x58454853 || tag == 0x52444853)
            return readDxbcProgram(data);
    throw std::runtime_error("Missing shader code");
}
std::vector<uint8_t> changeCode(Bytes bytes, const DxbcProgram &changed) {
    auto parts = readDxbcParts(bytes);
    auto data = writeDxbcProgram(changed);
    for (auto &[tag, payload] : parts)
        if (tag == 0x58454853 || tag == 0x52444853)
            payload = data;
    return makeDxbc(parts);
}
std::set<unsigned> uavRegisters(Bytes bytes) {
    dxbc_detail::Parts parts;
    for (const auto &[tag, data] : readDxbcParts(bytes))
        parts.emplace_back(tag, std::vector<uint8_t>(data.begin(), data.end()));
    return dxbc_detail::uavSlots(code(bytes), parts);
}
void save(const QString &name, Bytes bytes) {
    const auto root = qEnvironmentVariable("FLORA_FEEDBACK_RELOCATION_EVIDENCE");
    if (root.isEmpty())
        return;
    const auto path = QDir(root).filePath(QTest::currentDataTag());
    if (!QDir().mkpath(path))
        throw std::runtime_error("Cannot create feedback evidence directory");
    QFile file(QDir(path).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()) != qint64(bytes.size()))
        throw std::runtime_error("Feedback evidence requires a new writable path");
}
void save(const Json &report) {
    const auto text = report.dump(2);
    save("report.json", Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Json identity;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> adapter;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create feedback test factory");
            for (UINT i = 0;; ++i) {
                const auto hr = factory->EnumAdapters1(i, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    throw std::runtime_error("Requested feedback adapter not present");
                check(hr, "Enumerate feedback adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Read feedback adapter");
                if (desc.VendorId == vendor)
                    break;
                adapter.Reset();
            }
        }
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level{};
        check(D3D11CreateDevice(adapter.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, levels, 2, D3D11_SDK_VERSION, &device,
                                &level, &context),
              "Create feedback test device");
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> actual;
        check(device.As(&dxgi), "Query actual feedback device");
        check(dxgi->GetAdapter(&actual), "Read actual feedback adapter");
        DXGI_ADAPTER_DESC desc{};
        check(actual->GetDesc(&desc), "Read actual feedback identity");
        D3D11_FEATURE_DATA_D3D11_OPTIONS1 features{};
        check(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &features, sizeof(features)),
              "Read feedback capabilities");
        identity = {{"vendor", desc.VendorId},
                    {"device", desc.DeviceId},
                    {"luid_low", desc.AdapterLuid.LowPart},
                    {"luid_high", desc.AdapterLuid.HighPart},
                    {"warp", !vendor},
                    {"feature_level", uint32_t(level)},
                    {"tiled_resources_tier", uint32_t(features.TiledResourcesTier)}};
    }
    std::array<uint32_t, 8> dispatch(Bytes shader, uint32_t mode, bool srv, uint32_t slot) {
        Com<ID3D11ComputeShader> cs;
        check(device->CreateComputeShader(shader.data(), shader.size(), nullptr, &cs), "Create feedback CS");
        std::array<uint32_t, 32> input{};
        for (uint32_t i = 0; i < input.size(); ++i)
            input[i] = 100 + i;
        D3D11_BUFFER_DESC bd{sizeof(input),
                             D3D11_USAGE_DEFAULT,
                             srv ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_UNORDERED_ACCESS,
                             0,
                             mode == 1   ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                             : mode == 2 ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                         : 0u,
                             mode == 2 ? 16u : 0u};
        D3D11_SUBRESOURCE_DATA data{input.data(), 0, 0};
        Com<ID3D11Buffer> source, output, staging;
        check(device->CreateBuffer(&bd, &data, &source), "Create feedback source");
        bd = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
              16};
        std::array<uint32_t, 8> initial{};
        initial.fill(0xcdcdcdcd);
        data.pSysMem = initial.data();
        check(device->CreateBuffer(&bd, &data, &output), "Create feedback output");
        Com<ID3D11UnorderedAccessView> inputUav, outputUav;
        Com<ID3D11ShaderResourceView> inputSrv;
        if (srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = mode == 1   ? DXGI_FORMAT_R32_TYPELESS
                        : mode == 2 ? DXGI_FORMAT_UNKNOWN
                                    : DXGI_FORMAT_R32_UINT;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sd.BufferEx = {0, mode == 2 ? 8u : 32u, mode == 1 ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0u};
            check(device->CreateShaderResourceView(source.Get(), &sd, &inputSrv), "Create feedback SRV");
        } else {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = mode == 1   ? DXGI_FORMAT_R32_TYPELESS
                        : mode == 2 ? DXGI_FORMAT_UNKNOWN
                                    : DXGI_FORMAT_R32_UINT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer = {0, mode == 2 ? 8u : 32u, mode == 1 ? D3D11_BUFFER_UAV_FLAG_RAW : 0u};
            check(device->CreateUnorderedAccessView(source.Get(), &ud, &inputUav), "Create feedback UAV");
        }
        check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav), "Create output UAV");
        auto src = inputUav.Get();
        auto dst = outputUav.Get();
        auto read = inputSrv.Get();
        context->CSSetShader(cs.Get(), nullptr, 0);
        if (qEnvironmentVariableIsSet("FLORA_FEEDBACK_BATCH_BIND")) {
            std::array<ID3D11UnorderedAccessView *, 64> bound{};
            bound[slot] = src;
            bound[1] = dst;
            context->CSSetUnorderedAccessViews(0, UINT(bound.size()), bound.data(), nullptr);
        } else {
            context->CSSetUnorderedAccessViews(slot, 1, &src, nullptr);
            context->CSSetUnorderedAccessViews(1, 1, &dst, nullptr);
        }
        context->CSSetShaderResources(0, 1, &read);
        context->Dispatch(1, 1, 1);
        context->ClearState();
        bd = {sizeof(input), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        check(device->CreateBuffer(&bd, nullptr, &staging), "Create feedback staging");
        // One copy checks the input stayed intact; the other captures data and interpreted mapping status.
        auto readback = [&](ID3D11Buffer *buffer, UINT bytes) {
            D3D11_BOX box{0, 0, 0, bytes, 1, 1};
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, buffer, 0, &box);
            D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            Com<ID3D11Query> query;
            check(device->CreateQuery(&qd, &query), "Create feedback completion query");
            context->End(query.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (true) {
                const auto hr = context->GetData(query.Get(), nullptr, 0, 0);
                check(hr, "Wait for feedback test completion");
                if (hr == S_OK)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Feedback GPU test deadline");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            D3D11_MAPPED_SUBRESOURCE map{};
            while (true) {
                const auto hr =
                    context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
                    check(hr, "Read completed feedback result");
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Feedback readback deadline");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::vector<uint32_t> values(bytes / 4);
            std::memcpy(values.data(), map.pData, bytes);
            context->Unmap(staging.Get(), 0);
            return values;
        };
        const auto unchanged = readback(source.Get(), sizeof(input));
        if (!std::equal(input.begin(), input.end(), unchanged.begin(), unchanged.end()))
            throw std::runtime_error("Feedback load changed source storage");
        const auto values = readback(output.Get(), 32);
        Com<ID3D11InfoQueue> info;
        check(device.As(&info), "Read feedback validation messages");
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            check(info->GetMessage(i, nullptr, &size), "Read feedback message size");
            std::vector<uint8_t> storage(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(info->GetMessage(i, message, &size), "Read feedback validation message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        info->ClearStoredMessages();
        std::array<uint32_t, 8> result{};
        std::copy(values.begin(), values.end(), result.begin());
        return result;
    }
};
} // namespace
class DxbcFeedbackRelocationTests final : public QObject {
    Q_OBJECT
  private slots:
    void transforms_data() {
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<bool>("srv");
        QTest::addColumn<bool>("pixel");
        QTest::addColumn<bool>("stripped");
        for (auto mode : {0u, 1u, 2u})
            for (bool srv : {false, true})
                for (bool pixel : {false, true})
                    for (bool stripped : {false, true})
                        QTest::newRow(qPrintable(
                            QString("layout-%1-%2-%3-%4").arg(mode).arg(srv).arg(pixel).arg(stripped)))
                            << mode << srv << pixel << stripped;
    }
    void transforms() {
        QFETCH(uint32_t, mode);
        QFETCH(bool, srv);
        QFETCH(bool, pixel);
        QFETCH(bool, stripped);
        try {
            auto f = fixture(mode, srv, pixel);
            if (stripped) {
                Com<ID3DBlob> blob;
                check(
                    D3DStripShader(f.bytes.data(), f.bytes.size(), D3DCOMPILER_STRIP_REFLECTION_DATA, &blob),
                    "Strip feedback reflection");
                const auto *p = static_cast<const uint8_t *>(blob->GetBufferPointer());
                f.bytes.assign(p, p + blob->GetBufferSize());
            }
            const auto original = code(f.bytes);
            auto found = std::find_if(original.instructions.begin(), original.instructions.end(),
                                      [&](const auto &row) { return (row[0] & 2047) == f.opcode; });
            QVERIFY(found != original.instructions.end());
            const auto expected = srv ? std::set<unsigned>{1} : std::set<unsigned>{0, 1};
            QCOMPARE(uavRegisters(f.bytes), expected);
            Json report{{"mode", mode},
                        {"srv", srv},
                        {"pixel", pixel},
                        {"stripped", stripped},
                        {"opcode", f.opcode},
                        {"original_tokens", *found},
                        {"destinations", Json::array()}};
            save("original.dxbc", f.bytes);
            for (auto destination : {7u, 63u}) {
                auto moved = relocateShaderUavs(f.bytes, {{0, destination}}, 64);
                QCOMPARE(moved.declared, expected);
                QCOMPARE(uavRegisters(moved.bytes), srv ? expected : (std::set<unsigned>{1, destination}));
                const auto back = relocateShaderUavs(moved.bytes, {{destination, 0}}, 64);
                QCOMPARE(code(back.bytes).instructions, original.instructions);
                Com<ID3DBlob> text;
                check(D3DDisassemble(moved.bytes.data(), moved.bytes.size(), 0, nullptr, &text),
                      "Validate relocated feedback layout");
                save(QString("moved-%1.dxbc").arg(destination), moved.bytes);
                report["destinations"].push_back(destination);
            }
            if (!srv)
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, relocateShaderUavs(f.bytes, {{0, 1}}));
            if (pixel) {
                const auto reserved = reserveCoverageTarget(f.bytes, {0, 1}, 64);
                QCOMPARE(reserved.mapping, (std::map<uint32_t, uint32_t>{{0, 1}, {1, 2}}));
                QCOMPARE(uavRegisters(reserved.bytes),
                         srv ? std::set<unsigned>{2} : (std::set<unsigned>{1, 2}));
                const auto restored = relocateShaderUavs(reserved.bytes, {{1, 0}, {2, 1}}, 64);
                QCOMPARE(code(restored.bytes).instructions, original.instructions);
                save("reserved.dxbc", reserved.bytes);
            }
            if (!srv) {
                auto missing = original;
                std::erase_if(missing.instructions, [](const auto &r) {
                    const auto op = r[0] & 2047;
                    return op >= 156 && op <= 158 && r.at(2) == 0;
                });
                auto bad = changeCode(f.bytes, missing);
                QVERIFY_THROWS_EXCEPTION(std::exception, uavRegisters(bad));
                QVERIFY_THROWS_EXCEPTION(std::exception, relocateShaderUavs(bad, {{0, 7}}));
            }
            auto truncated = original;
            auto &row = truncated.instructions[size_t(found - original.instructions.begin())];
            row.pop_back();
            row[0] = (row[0] & 0x00ffffffu) | (uint32_t(row.size()) << 24);
            auto bad = changeCode(f.bytes, truncated);
            QVERIFY_THROWS_EXCEPTION(std::exception, uavRegisters(bad));
            QVERIFY_THROWS_EXCEPTION(std::exception, relocateShaderUavs(bad, {{0, 7}}));
            report["roundtrip_tokens_equal"] = true;
            report["truncation_rejected"] = true;
            save(report);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void gpu_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("mode");
        QTest::addColumn<bool>("srv");
        for (auto vendor : {0u, 4318u, 32902u})
            for (auto mode : {0u, 1u, 2u})
                for (bool srv : {false, true})
                    QTest::newRow(qPrintable(QString("gpu-%1-%2-%3").arg(vendor).arg(mode).arg(srv)))
                        << vendor << mode << srv;
    }
    void gpu() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, mode);
        QFETCH(bool, srv);
        try {
            auto f = fixture(mode, srv);
            Device device(vendor);
            const auto original = device.dispatch(f.bytes, mode, srv, 0);
            Json report{{"adapter", device.identity},
                        {"mode", mode},
                        {"srv", srv},
                        {"opcode", f.opcode},
                        {"original", original},
                        {"relocated", Json::array()},
                        {"scope", "non_tiled_buffer_feedback_relocation"},
                        {"batch_bind", qEnvironmentVariableIsSet("FLORA_FEEDBACK_BATCH_BIND")}};
            save("original.dxbc", f.bytes);
            for (auto slot : {7u, 63u}) {
                auto moved = relocateShaderUavs(f.bytes, {{0, slot}}, 64);
                auto result = device.dispatch(moved.bytes, mode, srv, slot);
                const auto direct = fixture(mode, srv, false, slot);
                const auto control = device.dispatch(direct.bytes, mode, srv, slot);
                report["relocated"].push_back(
                    {{"slot", slot}, {"result", result}, {"compiled_control", control}});
                save(QString("control-%1.dxbc").arg(slot), direct.bytes);
                save(QString("moved-%1.dxbc").arg(slot), moved.bytes);
            }
            report["input_unchanged"] = true;
            report["debug_errors"] = 0;
            save(report);
            const std::array<uint32_t, 4> expected = mode == 2 ? std::array<uint32_t, 4>{108, 109, 110, 111}
                                                     : mode == 0
                                                         ? std::array<uint32_t, 4>{102, 0, 0, 1}
                                                         : std::array<uint32_t, 4>{102, 102, 102, 102};
            QVERIFY(std::equal(expected.begin(), expected.end(), original.begin()));
            QCOMPARE(original[4], 1u);
            QCOMPARE(original[5], 99u);
            QCOMPARE(original[6], 0x0011e000u);
            QCOMPARE(original[7], 123u);
            for (const auto &row : report.at("relocated")) {
                QCOMPARE(row.at("result"), row.at("compiled_control"));
                QCOMPARE(row.at("result"), Json(original));
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcFeedbackRelocationTests)
#include "DxbcFeedbackRelocationTests.moc"
