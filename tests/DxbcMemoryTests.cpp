#include "application/DxbcExecution.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QtTest>
#include <d3d11sdklayers.h>
#include <dxgi.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words words{code};
    for (const auto &arg : args)
        words.insert(words.end(), arg.begin(), arg.end());
    words[0] |= uint32_t(words.size()) << 24;
    return words;
}
Words reg(uint32_t kind, uint32_t index, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), index};
}
Words source(uint32_t kind, uint32_t index, uint32_t swizzle = 0xe4) {
    return {0x100006 | (kind << 12) | (swizzle << 4), index};
}
Words imm(uint32_t value) { return {0x4001, value}; }
Program program(std::vector<Words> rows) {
    rows.insert(rows.begin(), op(104, {{4}}));
    return Program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50050, 0}, rows})}}));
}
std::vector<Step> execute(Lane &lane) {
    std::vector<Step> steps;
    while (!lane.finished())
        steps.push_back(lane.step());
    return steps;
}
std::vector<uint8_t> bytes(uint32_t size, uint32_t seed) {
    std::vector<uint8_t> result(size);
    for (size_t i = 0; i < size; ++i)
        result[i] = uint8_t(seed + i * 37);
    return result;
}
std::string shaderSource(bool structured) {
    const std::string prefix = "cbuffer Params:register(b0){uint4 p;}";
    return prefix +
           (structured ? "struct Record{uint4 a;uint2 b;};StructuredBuffer<Record> input:register(t0);"
                         "RWStructuredBuffer<Record> output:register(u0);[numthreads(1,1,1)]void main(){"
                         "output[p.y].a=input[p.x].a.wxyz^output[0].a;output[p.y].b=input[p.x].b+uint2(7,9);}"
                       : "ByteAddressBuffer input:register(t0);RWByteAddressBuffer output:register(u0);"
                         "[numthreads(1,1,1)]void main(){uint4 v=input.Load4(p.x)^output.Load4(p.y);"
                         "output.Store4(p.z,v.wxyz);}");
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create memory probe factory");
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> adapter;
                const auto hr = factory->EnumAdapters1(i, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate memory probe adapters");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Read memory probe adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested memory probe adapter is absent");
        }
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create memory probe device");
        Com<IDXGIDevice> dxgiDevice;
        Com<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        LARGE_INTEGER driver{};
        check(device.As(&dxgiDevice), "Read memory probe DXGI device");
        check(dxgiDevice->GetAdapter(&adapter), "Read memory probe adapter identity");
        check(adapter->GetDesc(&description), "Read memory probe adapter description");
        check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "Read memory probe driver");
        qInfo().noquote() << QString::fromStdString(
            nlohmann::json({{"adapter", QString::fromWCharArray(description.Description).toStdString()},
                            {"vendor_id", description.VendorId},
                            {"device_id", description.DeviceId},
                            {"luid_low", description.AdapterLuid.LowPart},
                            {"luid_high", description.AdapterLuid.HighPart},
                            {"driver_version", driver.QuadPart}})
                .dump());
    }
    std::vector<uint8_t> run(Bytes shader, bool structured, uint32_t unbound,
                             const std::array<uint32_t, 4> &params, Bytes input, Bytes output) {
        Com<ID3D11ComputeShader> compute;
        check(device->CreateComputeShader(shader.data(), shader.size(), nullptr, &compute),
              "Create memory probe CS");
        D3D11_BUFFER_DESC desc{UINT(input.size()),
                               D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                               0,
                               UINT(structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                               : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS),
                               structured ? 24u : 0u};
        D3D11_SUBRESOURCE_DATA data{input.data(), 0, 0};
        Com<ID3D11Buffer> sourceBuffer, targetBuffer, constants, staging;
        check(device->CreateBuffer(&desc, &data, &sourceBuffer), "Create memory probe input");
        data.pSysMem = output.data();
        check(device->CreateBuffer(&desc, &data, &targetBuffer), "Create memory probe output");
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srvDesc.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
        srvDesc.BufferEx.FirstElement = structured ? 1u : 4u;
        srvDesc.BufferEx.NumElements = structured ? 3u : 16u;
        srvDesc.BufferEx.Flags = structured ? 0u : D3D11_BUFFEREX_SRV_FLAG_RAW;
        Com<ID3D11ShaderResourceView> srv;
        check(device->CreateShaderResourceView(sourceBuffer.Get(), &srvDesc, &srv),
              "Create memory probe SRV");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Format = srvDesc.Format;
        uavDesc.Buffer.FirstElement = srvDesc.BufferEx.FirstElement;
        uavDesc.Buffer.NumElements = srvDesc.BufferEx.NumElements;
        uavDesc.Buffer.Flags = structured ? 0u : D3D11_BUFFER_UAV_FLAG_RAW;
        Com<ID3D11UnorderedAccessView> uav;
        check(device->CreateUnorderedAccessView(targetBuffer.Get(), &uavDesc, &uav),
              "Create memory probe UAV");
        desc = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        data.pSysMem = params.data();
        check(device->CreateBuffer(&desc, &data, &constants), "Create memory probe constants");
        desc = {UINT(output.size()), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        check(device->CreateBuffer(&desc, nullptr, &staging), "Create memory probe staging");
        auto *cb = constants.Get();
        auto *s = (unbound & 1) ? nullptr : srv.Get();
        auto *u = (unbound & 2) ? nullptr : uav.Get();
        context->CSSetShader(compute.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetShaderResources(0, 1, &s);
        context->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        context->Dispatch(1, 1, 1);
        context->ClearState();
        context->CopyResource(staging.Get(), targetBuffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read memory probe output");
        std::vector<uint8_t> result(output.size());
        std::memcpy(result.data(), mapped.pData, result.size());
        context->Unmap(staging.Get(), 0);
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read memory probe validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read memory probe message size");
            std::vector<uint8_t> storage(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(queue->GetMessage(i, message, &size), "Read memory probe message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
            if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING)
                qInfo() << "D3D11 warning:" << message->pDescription;
        }
        return result;
    }
};
} // namespace
class DxbcMemoryTests final : public QObject {
    Q_OBJECT
  private slots:
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("structured");
        QTest::addColumn<uint32_t>("variant");
        QTest::addColumn<uint32_t>("unbound");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (bool structured : {false, true})
                for (uint32_t variant : {0u, 1u, 2u, 3u, 4u})
                    for (uint32_t unbound : {0u, 1u, 2u, 3u})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4")
                                                     .arg(vendor)
                                                     .arg(structured ? "structured" : "raw")
                                                     .arg(variant)
                                                     .arg(unbound)))
                            << vendor << structured << variant << unbound;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, structured);
        QFETCH(uint32_t, variant);
        QFETCH(uint32_t, unbound);
        try {
            const auto shader = compileHlsl(shaderSource(structured), "cs_5_0").bytecode;
            const auto input = bytes(structured ? 120 : 128, 19), output = bytes(structured ? 120 : 128, 73);
            std::array<uint32_t, 4> params{};
            if (structured)
                params = variant == 0   ? std::array<uint32_t, 4>{0, 1, 0, 0}
                         : variant == 1 ? std::array<uint32_t, 4>{2, 2, 0, 0}
                                        : std::array<uint32_t, 4>{3, 3, 0, 0};
            else
                params = variant == 0   ? std::array<uint32_t, 4>{0, 16, 32, 0}
                         : variant == 1 ? std::array<uint32_t, 4>{60, 64, 60, 0}
                                        : std::array<uint32_t, 4>{0xfffffffc, 0, 0xfffffffc, 0};
            if (variant == 3)
                params = {structured ? UINT32_MAX : 0xfffffffcu, 0, 0, 0};
            if (variant == 4)
                params = structured ? std::array<uint32_t, 4>{0, UINT32_MAX, 0, 0}
                                    : std::array<uint32_t, 4>{0, 0, 0xfffffffc, 0};
            BufferView sourceView{1, structured ? 24u : 16u, structured ? 72u : 64u, structured ? 24u : 0u},
                targetView = sourceView;
            targetView.resource = 2;
            if (unbound & 1)
                sourceView = {};
            if (unbound & 2)
                targetView = {};
            Inputs inputs;
            inputs.constantBuffers[0] = {Value::known(params)};
            inputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(input)}, {2, Buffer::known(output)}},
                std::map<Register, BufferView>{{{7, {0}}, sourceView}, {{30, {0}}, targetView}});
            Program code(shader);
            Lane lane(code, inputs);
            const auto steps = execute(lane);
            Device device(vendor);
            const auto gpu = device.run(shader, structured, unbound, params, input, output);
            const auto &actual = inputs.memory->buffers().at(2);
            qInfo().noquote() << QString::fromStdString(nlohmann::json({{"vendor", vendor},
                                                                        {"structured", structured},
                                                                        {"variant", variant},
                                                                        {"unbound", unbound},
                                                                        {"params", params},
                                                                        {"cpu", actual.bytes},
                                                                        {"gpu", gpu}})
                                                            .dump());
            QCOMPARE(actual.bytes, gpu);
            QVERIFY(std::all_of(actual.valid.begin(), actual.valid.end(), [](uint8_t v) { return v == 1; }));
            QCOMPARE(inputs.memory->buffers().at(1).bytes, input);
            auto restored = inputs.memory->buffers();
            for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                for (auto change = step->memoryChanges.rbegin(); change != step->memoryChanges.rend();
                     ++change) {
                    auto &buffer = restored.at(change->resource);
                    QCOMPARE(
                        std::vector<uint8_t>(buffer.bytes.begin() + change->offset,
                                             buffer.bytes.begin() + change->offset + change->after.size()),
                        change->after);
                    std::copy(change->before.begin(), change->before.end(),
                              buffer.bytes.begin() + change->offset);
                    std::copy(change->beforeValid.begin(), change->beforeValid.end(),
                              buffer.valid.begin() + change->offset);
                }
            QCOMPARE(restored.at(2).bytes, output);
            QVERIFY(std::all_of(restored.at(2).valid.begin(), restored.at(2).valid.end(),
                                [](uint8_t v) { return v == 1; }));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void unknownValidityAndAliasing() {
        auto initial = Buffer::known(bytes(32, 11));
        initial.valid[9] = 0;
        Inputs inputs;
        inputs.memory = std::make_shared<Memory>(
            std::map<uint64_t, Buffer>{{1, initial}},
            std::map<Register, BufferView>{{{30, {0}}, {1, 0, 16, 0}}, {{30, {1}}, {1, 4, 24, 0}}});
        auto code = program({op(157, {reg(30, 0)}), op(157, {reg(30, 1)}),
                             op(165, {reg(0, 0), imm(4), source(30, 1, 0x1b)}),
                             op(166, {reg(30, 0, 3), imm(12), source(0, 0)}),
                             op(166, {reg(30, 0, 1), imm(0), source(0, 0, 0xff)}), op(62)});
        Lane lane(code, inputs);
        const auto steps = execute(lane);
        QCOMPARE(lane.value({0, {0}}).valid, uint8_t(7));
        QCOMPARE(steps[1].memoryChanges.size(), size_t(1));
        QCOMPARE(steps[1].memoryChanges[0].after.size(), size_t(4));
        QCOMPARE(inputs.memory->buffers().at(1).bytes[12], initial.bytes[20]);
        for (size_t i = 0; i < 4; ++i)
            QCOMPARE(inputs.memory->buffers().at(1).valid[i], uint8_t(0));
        auto missing = program({op(161, {reg(7, 0)}), op(165, {reg(0, 0), imm(0), source(7, 0)}), op(62)});
        Lane unknown(missing, {});
        execute(unknown);
        QCOMPARE(unknown.value({0, {0}}).valid, uint8_t(0));
    }
    void undefinedFieldsAndFailureRollback() {
        const auto initial = Buffer::known(bytes(64, 23));
        Inputs inputs;
        inputs.memory =
            std::make_shared<Memory>(std::map<uint64_t, Buffer>{{1, initial}},
                                     std::map<Register, BufferView>{{{30, {0}}, {1, 16, 32, 16}}});
        auto load = program(
            {op(158, {reg(30, 0), {16}}), op(167, {reg(0, 0), imm(0), imm(12), source(30, 0)}), op(62)});
        Lane read(load, inputs);
        execute(read);
        QCOMPARE(read.value({0, {0}}).valid, uint8_t(0));
        auto store =
            program({op(158, {reg(30, 0), {16}}), op(168, {reg(30, 0), imm(0), imm(12), imm(0)}), op(62)});
        Lane write(store, inputs);
        execute(write);
        const auto &buffer = inputs.memory->buffers().at(1);
        for (size_t i = 0; i < 64; ++i)
            QCOMPARE(buffer.valid[i], uint8_t(i < 16 || i >= 48));
        QCOMPARE(buffer.bytes, initial.bytes);
        auto noReturn = program({op(157, {reg(30, 0)}), op(166, {reg(30, 0), imm(0), imm(7)})});
        Inputs raw;
        raw.memory = std::make_shared<Memory>(std::map<uint64_t, Buffer>{{1, initial}},
                                              std::map<Register, BufferView>{{{30, {0}}, {1, 0, 64, 0}}});
        Lane failing(noReturn, raw);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, failing.step());
        QCOMPARE(raw.memory->buffers().at(1).bytes, initial.bytes);
        QVERIFY(failing.registers().empty());
    }
    void validationAndBudgets() {
        const auto initial = Buffer::known(bytes(64, 31));
        using Buffers = std::map<uint64_t, Buffer>;
        using Views = std::map<Register, BufferView>;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Memory(Buffers{{1, initial}}, Views{}, 32));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 Memory(Buffers{{1, initial}}, Views{{{7, {0}}, {1, 60, 8, 0}}}));
        auto invalid = initial;
        invalid.valid[0] = 2;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Memory(Buffers{{1, invalid}}, Views{}));
        Inputs inputs;
        inputs.memory =
            std::make_shared<Memory>(Buffers{{1, initial}}, Views{{{30, {0}}, {1, 0, 64, 16}}}, 64, 16);
        auto code =
            program({op(158, {reg(30, 0), {16}}), op(168, {reg(30, 0), imm(0), imm(12), imm(0)}), op(62)});
        Lane budget(code, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, budget.step());
        QCOMPARE(inputs.memory->buffers().at(1).valid, initial.valid);
        auto address = program(
            {op(158, {reg(30, 0), {16}}), op(168, {reg(30, 0), source(0, 0, 0), imm(0), imm(0)}), op(62)});
        Lane unknown(address, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknown.step());
        QCOMPARE(inputs.memory->buffers().at(1).bytes, initial.bytes);
        auto stride = program(
            {op(158, {reg(30, 0), {32}}), op(167, {reg(0, 0), imm(0), imm(0), source(30, 0)}), op(62)});
        Lane wrongStride(stride, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, wrongStride.step());
        QVERIFY(wrongStride.registers().empty());
        auto mask = program({op(157, {reg(30, 0)}), op(166, {reg(30, 0, 5), imm(0), imm(0)}), op(62)});
        Inputs raw;
        raw.memory = std::make_shared<Memory>(Buffers{{1, initial}}, Views{{{30, {0}}, {1, 0, 64, 0}}});
        Lane invalidMask(mask, raw);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalidMask.step());
        QCOMPARE(raw.memory->buffers().at(1).bytes, initial.bytes);
    }
};
QTEST_GUILESS_MAIN(DxbcMemoryTests)
#include "DxbcMemoryTests.moc"
