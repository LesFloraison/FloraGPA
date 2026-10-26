#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <d3d11sdklayers.h>
#include <dxgi.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
using Constants = std::array<std::array<uint32_t, 4>, 4>;
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words precision(Words arg, uint32_t value) {
    if (value) {
        arg[0] |= 0x80000000u;
        arg.insert(arg.begin() + 1, 1u | (value << 14));
    }
    return arg;
}
Words output(uint32_t slot, uint32_t mask, uint32_t bits) {
    return precision({0x102002 | mask << 4, slot}, bits);
}
std::vector<uint8_t> fixture(uint32_t bits, uint32_t opcode, uint32_t slot, uint32_t mask,
                             bool sourcePrecision, bool refactoring) {
    const std::map<uint32_t, std::string> types{
        {1, "min16float"}, {2, "min10float"}, {4, "min16int"}, {5, "min16uint"}};
    const auto compiled =
        compileHlsl(types.at(bits) + "4 main():SV_Target" + std::to_string(slot) + "{return 0;}", "ps_5_0")
            .bytecode;
    const auto source = [&](uint32_t index) {
        return precision(Words{0x208e46, 0, index}, sourcePrecision ? bits : 0);
    };
    std::vector<Words> rows{row(106 | (1u << 16) | (refactoring ? 1u << 11 : 0u)),
                            row(89, {{0x208e46, 0, 4}}), row(101, {output(slot, 15, bits)}), row(104, {{1}})};
    if (mask != 15)
        rows.push_back(row(54, {output(slot, 15 ^ mask, bits), {0x208e46, 0, 3}}));
    const auto op = opcode & 2047;
    if (op == 54 || op == 75)
        rows.push_back(row(opcode, {output(slot, mask, bits), source(0)}));
    else if (op == 50 || op == 55)
        rows.push_back(row(opcode, {output(slot, mask, bits), source(0), source(1), source(2)}));
    else
        rows.push_back(row(opcode, {output(slot, mask, bits), source(0), source(1)}));
    rows.push_back(row(62));
    const auto code = writeDxbcProgram({{0x50, 0}, rows});
    auto parts = readDxbcParts(compiled);
    std::erase_if(parts, [](const auto &p) {
        return p.first == 0x58454853 || p.first == 0x52444853 || p.first == 0x54415453;
    });
    parts.emplace_back(0x58454853, code);
    return makeDxbc(parts);
}
std::string hash(Bytes data) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11RenderTargetView> outputView;
    DXGI_ADAPTER_DESC identity{};
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create output reference factory");
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> adapter;
                const auto hr = factory->EnumAdapters1(i, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate output reference adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Read output adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested output adapter is absent");
        }
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create output reference device");
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> adapter;
        check(device.As(&dxgi), "Read output DXGI device");
        check(dxgi->GetAdapter(&adapter), "Read actual output adapter");
        check(adapter->GetDesc(&identity), "Read output adapter identity");
    }
    void validate() {
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read output validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read output validation size");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size), "Read output validation message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        queue->ClearStoredMessages();
    }
    std::array<uint32_t, 4> run(Bytes code, const Constants &constants, uint32_t precision, uint32_t slot) {
        const auto vsCode =
            compileHlsl("float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);"
                        "return float4(p*float2(2,-2)+float2(-1,1),0,1);}",
                        "vs_5_0")
                .bytecode;
        Com<ID3D11VertexShader> vertex;
        Com<ID3D11PixelShader> pixel;
        check(device->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, &vertex), "Create output VS");
        check(device->CreatePixelShader(code.data(), code.size(), nullptr, &pixel), "Create output PS");
        D3D11_TEXTURE2D_DESC desc{1,
                                  1,
                                  1,
                                  1,
                                  precision < 4    ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                  : precision == 4 ? DXGI_FORMAT_R32G32B32A32_SINT
                                                   : DXGI_FORMAT_R32G32B32A32_UINT,
                                  {1, 0},
                                  D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET,
                                  0,
                                  0};
        Com<ID3D11Texture2D> target, staging;
        check(device->CreateTexture2D(&desc, nullptr, &target), "Create output target");
        check(device->CreateRenderTargetView(target.Get(), nullptr, &outputView), "Create output view");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Create output staging");
        D3D11_BUFFER_DESC cbDesc{
            sizeof constants, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA initial{constants.data(), 0, 0};
        Com<ID3D11Buffer> cb;
        check(device->CreateBuffer(&cbDesc, &initial, &cb), "Create output constants");
        std::array<ID3D11RenderTargetView *, 8> views{};
        views[slot] = outputView.Get();
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(slot + 1, views.data(), nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read output reference");
        std::array<uint32_t, 4> result;
        std::memcpy(result.data(), mapped.pData, 16);
        context->Unmap(staging.Get(), 0);
        validate();
        return result;
    }
    bool preserved(uint32_t slot) {
        std::array<ID3D11RenderTargetView *, 8> views{};
        context->OMGetRenderTargets(8, views.data(), nullptr);
        const bool same = views[slot] == outputView.Get();
        for (auto *view : views)
            if (view)
                view->Release();
        return same;
    }
};
} // namespace
class DxbcPixelOutputTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
  private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("FLORA_KEEP_PIXEL_OUTPUT_EVIDENCE")) {
            evidence.setAutoRemove(false);
            qInfo().noquote() << "Pixel output evidence:" << evidence.path();
        }
    }
    void precision_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("bits");
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<uint32_t>("slot");
        QTest::addColumn<uint32_t>("mask");
        QTest::addColumn<bool>("sourcePrecision");
        QTest::addColumn<bool>("refactoring");
        QTest::addColumn<uint32_t>("dataset");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t bits : {1u, 2u, 4u, 5u}) {
                const auto operations = bits < 4
                                            ? Words{54, 0, 56, 50, 16, 75, 54 | 0x2000}
                                            : Words{54, 30, bits == 4 ? 36u : 83u, bits == 4 ? 37u : 84u, 55};
                for (auto opcode : operations)
                    for (uint32_t slot : {0u, 3u, 7u})
                        for (uint32_t mask : {15u, 5u, 10u})
                            for (bool sourcePrecision : {false, true})
                                for (bool refactoring : {false, true})
                                    for (uint32_t dataset : {0u, 1u, 2u, 3u}) {
                                        if (dataset && (opcode != 54 || mask != 15))
                                            continue;
                                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5-%6-%7-%8")
                                                                     .arg(vendor)
                                                                     .arg(bits)
                                                                     .arg(opcode)
                                                                     .arg(slot)
                                                                     .arg(mask)
                                                                     .arg(sourcePrecision)
                                                                     .arg(refactoring)
                                                                     .arg(dataset)))
                                            << vendor << bits << opcode << slot << mask << sourcePrecision
                                            << refactoring << dataset;
                                    }
            }
    }
    void precision() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, bits);
        QFETCH(uint32_t, opcode);
        QFETCH(uint32_t, slot);
        QFETCH(uint32_t, mask);
        QFETCH(bool, sourcePrecision);
        QFETCH(bool, refactoring);
        QFETCH(uint32_t, dataset);
        try {
            Constants constants = bits < 4 ? Constants{{{0x3eaaaaab, 0xbf000000, 0x3f801001, 0x80000000},
                                                        {0x3f000000, 0x3e800000, 0x3f000001, 0x3eaaaaab},
                                                        {0x3e000000, 0x3f000000, 0xbf000000, 0},
                                                        {0x3e800000, 0x3f400000, 0x3f000000, 0}}}
                                           : Constants{{{0, 17, bits == 4 ? uint32_t(-19) : 19, 127},
                                                        {7, 15, 23, 5},
                                                        {31, 37, 41, 43},
                                                        {47, 53, 59, 61}}};
            if (dataset) {
                const std::array<std::array<uint32_t, 4>, 3> special =
                    bits < 4
                        ? std::array<std::array<uint32_t, 4>, 3>{{{0, 0x80000000, 0x38800000, 0xb8800000},
                                                                  {0x7fc12345, 0x7f800000, 0xff800000,
                                                                   0x7f812345},
                                                                  {1, 0x80000001, 0x007fffff, 0x807fffff}}}
                        : std::array<std::array<uint32_t, 4>, 3>{{{0, 32767, 32768, 65535},
                                                                  {65536, UINT32_MAX, 0x80000000, 0x7fffffff},
                                                                  {17, uint32_t(-17), 0, 1}}};
                constants[0] = special.at(dataset - 1);
            }
            const auto code = fixture(bits, opcode, slot, mask, sourcePrecision, refactoring);
            Device device(vendor);
            const auto reference = device.run(code, constants, bits, slot);
            Inputs inputs;
            inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), vendor == 0);
            for (const auto &value : constants)
                inputs.constantBuffers[0].push_back(Value::known(value));
            Program program(code);
            Lane lane(program, inputs);
            std::vector<Step> trace;
            while (!lane.finished())
                trace.push_back(lane.step());
            const auto actual = lane.value({2, {slot}});
            nlohmann::json report{{"scope", "native_pixel_output_precision"},
                                  {"vendor", vendor},
                                  {"precision", bits},
                                  {"opcode", opcode},
                                  {"slot", slot},
                                  {"mask", mask},
                                  {"dataset", dataset},
                                  {"source_precision", sourcePrecision},
                                  {"refactoring_allowed", refactoring},
                                  {"shader_sha256", hash(code)},
                                  {"constants", constants},
                                  {"actual", actual.bits},
                                  {"expected", reference},
                                  {"valid", actual.valid},
                                  {"helper", inputs.gpu->identity()},
                                  {"reference_adapter",
                                   {{"vendor_id", device.identity.VendorId},
                                    {"device_id", device.identity.DeviceId},
                                    {"luid_low", device.identity.AdapterLuid.LowPart},
                                    {"luid_high", device.identity.AdapterLuid.HighPart}}}};
            const auto name = QString::fromLatin1(QTest::currentDataTag());
            QFile shader(evidence.filePath(name + ".dxbc"));
            QVERIFY(shader.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(shader.write(reinterpret_cast<const char *>(code.data()), qint64(code.size())),
                     qint64(code.size()));
            QFile json(evidence.filePath(name + ".json"));
            QVERIFY(json.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto text = report.dump(2);
            QCOMPARE(json.write(text.data(), qint64(text.size())), qint64(text.size()));
            qInfo().noquote() << QString::fromStdString(report.dump());
            QCOMPARE(actual.valid, uint8_t(15));
            QCOMPARE(actual.bits, reference);
            QVERIFY(device.preserved(slot));
            auto registers = lane.registers();
            for (auto step = trace.rbegin(); step != trace.rend(); ++step)
                for (auto change = step->changes.rbegin(); change != step->changes.rend(); ++change)
                    registers[change->destination] = change->before;
            for (const auto &[reg, value] : registers) {
                QCOMPARE(value.valid, uint8_t(0));
                QCOMPARE(value.bits, (std::array<uint32_t, 4>{}));
            }
            device.validate();
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void validityAndCancellation() {
        try {
            Device device(0);
            bool cancelled = false;
            Inputs inputs;
            inputs.gpu =
                std::make_shared<GpuOperations>(device.device.Get(), true, [&] { return cancelled; });
            inputs.constantBuffers[0] = {
                {{0x3f800000, 0, 0x3f000000, 0}, 1}, {}, {}, Value::known({0, 0x3e800000, 0, 0x3f400000})};
            const auto code = fixture(1, 54, 7, 5, true, true);
            const Program program(code);
            Lane lane(program, inputs);
            lane.step();
            const auto before = lane.registers();
            const auto count = inputs.gpu->operations();
            cancelled = true;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, lane.step());
            QCOMPARE(lane.registers(), before);
            QCOMPARE(inputs.gpu->operations(), count);
            cancelled = false;
            const auto step = lane.step();
            QVERIFY(step.instructionGpuAssisted);
            const auto actual = lane.value({2, {7}});
            QCOMPARE(actual.valid, uint8_t(11));
            QCOMPARE(actual.bits[0], 0x3f800000u);
            QCOMPARE(actual.bits[1], 0x3e800000u);
            QCOMPARE(actual.bits[3], 0x3f400000u);
            QCOMPARE(step.changes.at(0).before, before.at({2, {7}}));
            device.validate();
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcPixelOutputTests)
#include "DxbcPixelOutputTests.moc"
