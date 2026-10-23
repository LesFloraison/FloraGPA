#include "application/DxbcCheckpointModel.h"
#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QtTest>
#include <bit>
#include <cmath>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <immintrin.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
Words op(uint32_t opcode, std::initializer_list<Words> operands = {}) {
    Words result{opcode};
    for (const auto &arg : operands)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words dst(uint32_t kind, uint32_t reg, uint32_t mask = 15) {
    return {(kind << 12) | 0x100002 | (mask << 4), reg};
}
Words src(uint32_t kind, uint32_t reg) { return {(kind << 12) | 0x100e46, reg}; }
Words literal(uint32_t value) { return {0x4001, value}; }
std::vector<uint8_t> binary(std::vector<Words> rows) {
    rows.insert(rows.begin(), {op(104, {{4}}), op(101, {dst(2, 0)})});
    const auto bytes = writeDxbcProgram({{0x10050, 0}, rows});
    return makeDxbc({{0x58454853, bytes}});
}
std::vector<uint8_t> gpuBinary(std::vector<Words> rows) {
    const auto container = compileHlsl("uint4 main():TEXCOORD0{return 0;}", "vs_5_0").bytecode;
    const auto code = binary(std::move(rows));
    auto parts = readDxbcParts(container);
    std::erase_if(parts, [](const auto &p) {
        return p.first == 0x58454853 || p.first == 0x52444853 || p.first == 0x54415453;
    });
    parts.emplace_back(0x58454853, readDxbcParts(code)[0].second);
    return makeDxbc(parts);
}
Words vectorLiteral(std::array<uint32_t, 4> bits) { return {0x4002, bits[0], bits[1], bits[2], bits[3]}; }
std::vector<Words> rawFixture(uint32_t opcode, uint32_t output, bool aliased) {
    const auto a = vectorLiteral({UINT32_MAX, 0x80000000, 0x10000000, 0}), b = vectorLiteral({2, 3, 33, 0});
    if (aliased)
        return {op(opcode, {dst(2, 0, 3), dst(2, 0, 12), a, b}), op(62)};
    if (opcode == 38 || opcode == 81 || opcode == 132 || opcode == 133)
        return {op(opcode, {dst(0, 0), dst(0, 1), a, b}), op(54, {dst(2, 0), src(0, output)}), op(62)};
    if (opcode == 142)
        return {op(opcode, {dst(0, 0), dst(0, 1), vectorLiteral({1, 0, 1, 0}), a, b}),
                op(54, {dst(2, 0), src(0, output)}), op(62)};
    if (opcode == 138 || opcode == 139)
        return {op(opcode, {dst(2, 0), vectorLiteral({0, 31, 31, 8}), vectorLiteral({0, 8, 31, 4}), a}),
                op(62)};
    if (opcode == 140)
        return {op(opcode, {dst(2, 0), vectorLiteral({0, 31, 31, 8}), vectorLiteral({0, 8, 31, 4}), a, b}),
                op(62)};
    if (opcode == 130)
        return {op(opcode, {dst(2, 0), vectorLiteral({0x3f801000, 0x3f803000, 0x33000000, 0x33800000})}),
                op(62)};
    if (opcode == 131)
        return {op(opcode, {dst(2, 0), vectorLiteral({0x8000, 1, 0x3ff, 0x7bff})}), op(62)};
    if (opcode == 27 || opcode == 28)
        return {op(opcode, {dst(2, 0), vectorLiteral({0x7fc12345, 0xff800000, 0x7f800000, 0x4f000000})}),
                op(62)};
    return {op(opcode, {dst(2, 0), a}), op(62)};
}
std::vector<Step> execute(Lane &lane) {
    std::vector<Step> steps;
    while (!lane.finished())
        steps.push_back(lane.step());
    return steps;
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create execution probe factory");
            for (UINT index = 0;; ++index) {
                Com<IDXGIAdapter1> adapter;
                const auto hr = factory->EnumAdapters1(index, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate execution probe adapters");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Read execution probe adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested execution probe adapter is absent");
        }
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, levels, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create execution probe device");
        Com<IDXGIDevice> dxgiDevice;
        Com<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        check(device.As(&dxgiDevice), "Read execution probe DXGI device");
        check(dxgiDevice->GetAdapter(&adapter), "Read execution probe adapter");
        check(adapter->GetDesc(&description), "Read execution probe adapter identity");
        LARGE_INTEGER driver{};
        check(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver), "Read execution probe driver");
        qInfo().noquote() << QString::fromStdString(
            nlohmann::json({{"adapter", QString::fromWCharArray(description.Description).toStdString()},
                            {"vendor_id", description.VendorId},
                            {"device_id", description.DeviceId},
                            {"luid_low", description.AdapterLuid.LowPart},
                            {"luid_high", description.AdapterLuid.HighPart},
                            {"driver_version", driver.QuadPart}})
                .dump());
    }
    std::array<uint32_t, 4> run(Bytes shader, std::span<const uint32_t> constants) {
        Com<ID3D11VertexShader> vertex;
        check(device->CreateVertexShader(shader.data(), shader.size(), nullptr, &vertex),
              "Create execution oracle VS");
        const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 0, 0, 4, 0};
        const UINT stride = 16;
        Com<ID3D11GeometryShader> outputShader;
        check(device->CreateGeometryShaderWithStreamOutput(shader.data(), shader.size(), &entry, 1, &stride,
                                                           1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
                                                           &outputShader),
              "Create execution oracle stream output");
        std::vector<uint32_t> padded(constants.begin(), constants.end());
        padded.resize(std::max(size_t(4), (padded.size() + 3) & ~size_t(3)));
        D3D11_BUFFER_DESC desc{
            UINT(padded.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA initial{padded.data(), 0, 0};
        Com<ID3D11Buffer> cb, output, staging;
        check(device->CreateBuffer(&desc, &initial, &cb), "Create execution oracle constants");
        desc = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        check(device->CreateBuffer(&desc, nullptr, &output), "Create execution oracle output");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateBuffer(&desc, nullptr, &staging), "Create execution oracle staging");
        auto *constant = cb.Get();
        auto *target = output.Get();
        const UINT offset = 0;
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &constant);
        context->GSSetShader(outputShader.Get(), nullptr, 0);
        context->SOSetTargets(1, &target, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        context->Draw(1, 0);
        context->ClearState();
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read execution oracle output");
        std::array<uint32_t, 4> result;
        std::memcpy(result.data(), mapped.pData, 16);
        context->Unmap(staging.Get(), 0);
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read execution oracle validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T bytes = 0;
            check(queue->GetMessage(i, nullptr, &bytes), "Get oracle message size");
            std::vector<uint8_t> storage(bytes);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(queue->GetMessage(i, message, &bytes), "Get oracle message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
            if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING)
                qInfo() << "D3D11 warning:" << message->pDescription;
        }
        return result;
    }
};
struct Fixture {
    const char *name, *body;
    bool floating = false;
    double tolerance = 0;
};
const Fixture fixtures[]{
    {"unsigned-add", "return a+b;"},
    {"multiply-add", "return a*b+c;"},
    {"signed-comparison", "return uint4(asint(a.x)<asint(b.x),asint(a.y)>=asint(b.y),a.z==b.z,a.w!=b.w);"},
    {"minmax",
     "return uint4(min(asint(a.x),asint(b.x)),max(asint(a.y),asint(b.y)),min(a.z,b.z),max(a.w,b.w));"},
    {"division", "return uint4(a.x/b.x,a.y%b.y,a.z/b.z,a.w%b.w);"},
    {"shift-bits", "return uint4(asint(a.x)>>b.x,a.y<<b.y,a.z>>b.z,(a.w^b.w)|(~a.x&b.x));"},
    {"bit-intrinsics", "return uint4(countbits(a.x),firstbithigh(a.y),firstbitlow(a.z),reversebits(a.w));"},
    {"signed-firstbit", "return firstbithigh(asint(a));"},
    {"conditional", "[branch]if(a.x&1)return a.zyxw+b.wzyx;else return c.wxyz-a;"},
    {"loop-continue-break",
     "uint4 x=a;[loop]for(uint "
     "i=0;i<(b.x&15);++i){if(i==3)continue;if(i==9)break;x=x*3+uint4(i,i+1,i+2,i+3);}return x;"},
    {"switch", "uint4 x=a;[forcecase]switch(a.x&3){case 0:x+=b;break;case 1:case "
               "2:x^=c;break;default:x*=b;break;}return x;"},
    {"indexable",
     "uint4 values[16];[loop]for(uint i=0;i<16;++i)values[i]=a+i;values[b.x&15]^=c;return values[a.x&15];"},
    {"float-add", "return asuint(asfloat(a)+asfloat(b));", true},
    {"float-multiply", "return asuint(asfloat(a)*asfloat(b));", true},
    {"float-mad", "return asuint(asfloat(a)*asfloat(b)+asfloat(c));", true},
    {"float-round",
     "return asuint(float4(round(asfloat(a.x)),floor(asfloat(a.y)),ceil(asfloat(a.z)),trunc(asfloat(a.w))));",
     true},
    {"float-modifiers", "return asuint(saturate(-abs(asfloat(a))+asfloat(b)));", true},
    {"float-compare",
     "return "
     "uint4(asfloat(a.x)<asfloat(b.x),asfloat(a.y)>=asfloat(b.y),asfloat(a.z)==asfloat(b.z),asfloat(a.w)!="
     "asfloat(b.w));",
     true},
    {"float-dot", "float v=dot(asfloat(a),asfloat(b));return asuint(v.xxxx);", true},
    {"float-convert",
     "return uint4(int(asfloat(a.x)),uint(asfloat(a.y)),int(asfloat(a.z)),uint(asfloat(a.w)));", true},
    {"integer-convert", "return asuint(float4(asint(a)));"},
    {"float-exp-log",
     "return "
     "asuint(float4(exp2(asfloat(a.x)),log2(abs(asfloat(a.y))),rcp(asfloat(a.z)),sqrt(abs(asfloat(a.w)))));",
     true, 0x1p-20},
};
} // namespace
class DxbcExecutionTests final : public QObject {
    Q_OBJECT
  private slots:
    void rawInstructions_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<uint32_t>("form");
        QTest::addColumn<uint32_t>("output");
        QTest::addColumn<bool>("aliased");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t opcode : {27u, 28u, 38u, 81u, 130u, 131u, 132u, 133u, 134u, 135u, 136u, 137u, 138u,
                                    139u, 140u, 141u, 142u})
                for (uint32_t form : {0u, 1u, 2u})
                    for (uint32_t output = 0; output < (opcode == 38 || opcode == 81 || opcode == 132 ||
                                                                opcode == 133 || opcode == 142
                                                            ? 2u
                                                            : 1u);
                         ++output)
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4")
                                                     .arg(vendor)
                                                     .arg(opcode)
                                                     .arg(form == 1   ? "cb"
                                                          : form == 2 ? "scalar"
                                                                      : "literal")
                                                     .arg(output)))
                            << vendor << opcode << form << output << false;
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t opcode : {38u, 81u, 132u, 133u})
                for (uint32_t form : {0u, 1u})
                    QTest::newRow(qPrintable(
                        QString("%1-%2-%3-alias").arg(vendor).arg(opcode).arg(form == 1 ? "cb" : "literal")))
                        << vendor << opcode << form << 0u << true;
    }
    void rawInstructions() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, opcode);
        QFETCH(uint32_t, form);
        QFETCH(uint32_t, output);
        QFETCH(bool, aliased);
        try {
            auto rows = rawFixture(opcode, output, aliased);
            Inputs inputs;
            std::vector<uint32_t> constants;
            if (form == 2) {
                std::vector<Words> scalarRows;
                for (const auto &row : rows) {
                    const auto parsed = checkpoint::instructionOperands(row);
                    const bool hasLiteral =
                        std::any_of(parsed.ranges.begin(), parsed.ranges.end(),
                                    [&](const auto &range) { return row[range.first] == 0x4002; });
                    if (!hasLiteral) {
                        scalarRows.push_back(row);
                        continue;
                    }
                    for (uint32_t component = 0; component < 4; ++component) {
                        auto replaced = parsed.prefix;
                        for (const auto &[begin, end] : parsed.ranges) {
                            if (row[begin] == 0x4002)
                                replaced.insert(replaced.end(), {0x4001, row[begin + 1 + component]});
                            else {
                                auto arg = Words(row.begin() + begin, row.begin() + end);
                                arg[0] = (arg[0] & ~0xf0u) | (1u << (4 + component));
                                replaced.insert(replaced.end(), arg.begin(), arg.end());
                            }
                        }
                        replaced[0] = (replaced[0] & 0x80ffffff) | (uint32_t(replaced.size()) << 24);
                        scalarRows.push_back(std::move(replaced));
                    }
                }
                rows = std::move(scalarRows);
            }
            if (form == 1) {
                for (auto &row : rows) {
                    const auto parsed = checkpoint::instructionOperands(row);
                    auto replaced = parsed.prefix;
                    for (const auto &[begin, end] : parsed.ranges) {
                        if (row[begin] == 0x4002) {
                            const auto index = uint32_t(constants.size() / 4);
                            replaced.insert(replaced.end(), {0x00208e46, 0, index});
                            constants.insert(constants.end(), row.begin() + begin + 1, row.begin() + end);
                            inputs.constantBuffers[0].push_back(Value::known(
                                {row[begin + 1], row[begin + 2], row[begin + 3], row[begin + 4]}));
                        } else
                            replaced.insert(replaced.end(), row.begin() + begin, row.begin() + end);
                    }
                    replaced[0] = (replaced[0] & 0x80ffffff) | (uint32_t(replaced.size()) << 24);
                    row = std::move(replaced);
                }
                rows.insert(rows.begin(), op(89, {{0x00208e46, 0, uint32_t(constants.size() / 4)}}));
            }
            const auto shader = gpuBinary(rows);
            Com<ID3DBlob> disassembly;
            check(D3DDisassemble(shader.data(), shader.size(), 0, nullptr, &disassembly),
                  "Disassemble raw execution probe");
            qInfo().noquote() << QString::fromUtf8(static_cast<const char *>(disassembly->GetBufferPointer()),
                                                   int(disassembly->GetBufferSize()));
            Program program(shader);
            Device device(vendor);
            const std::array<uint32_t, 4> sentinelBits{11, 22, 33, 44};
            const D3D11_BUFFER_DESC sentinelDesc{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0,
                                                 0};
            const D3D11_SUBRESOURCE_DATA sentinelData{sentinelBits.data(), 0, 0};
            Com<ID3D11Buffer> sentinel;
            check(device.device->CreateBuffer(&sentinelDesc, &sentinelData, &sentinel),
                  "Create instruction sentinel");
            auto *binding = sentinel.Get();
            device.context->VSSetConstantBuffers(13, 1, &binding);
            inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), vendor == 0);
            Lane lane(program, inputs);
            const auto steps = execute(lane);
            Com<ID3D11Buffer> preserved;
            device.context->VSGetConstantBuffers(13, 1, &preserved);
            QCOMPARE(preserved.Get(), sentinel.Get());
            const auto assisted = std::count_if(steps.begin(), steps.end(),
                                                [](const Step &step) { return step.instructionGpuAssisted; });
            QVERIFY(inputs.gpu->operations() >= uint64_t(assisted));
            QVERIFY(inputs.gpu->operations() <= uint64_t(assisted) * 2);
            auto restored = lane.registers();
            for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                for (auto change = step->changes.rbegin(); change != step->changes.rend(); ++change) {
                    QCOMPARE(restored[change->destination], change->after);
                    restored[change->destination] = change->before;
                }
            for (const auto &[reg, value] : restored)
                QCOMPARE(value, inputs.registers.contains(reg) ? inputs.registers.at(reg) : Value{});
            const auto gpu = device.run(shader, constants);
            const auto cpu = lane.value({2, {0}});
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"cpu", cpu.bits},
                                {"gpu", gpu},
                                {"opcode", opcode},
                                {"vendor", vendor},
                                {"form", form},
                                {"aliased", aliased},
                                {"assistance", inputs.gpu->identity()},
                                {"gpu_operations", inputs.gpu->operations()},
                                {"output", output}})
                    .dump());
            QCOMPARE(cpu.valid, uint8_t(15));
            for (size_t i = 0; i < 4; ++i)
                QCOMPARE(cpu.bits[i], gpu[i]);
            if (opcode == 135)
                QCOMPARE(cpu.bits[2], 3u);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("fixture");
        QTest::addColumn<uint32_t>("variant");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t i = 0; i < std::size(fixtures); ++i)
                for (uint32_t variant = 0; variant < (fixtures[i].floating ? 1u : 3u); ++variant)
                    QTest::newRow(
                        qPrintable(QString("%1-%2-%3").arg(vendor).arg(fixtures[i].name).arg(variant)))
                        << vendor << i << variant;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, fixture);
        QFETCH(uint32_t, variant);
        try {
            const auto &f = fixtures[fixture];
            const auto shader =
                compileHlsl(
                    std::string(
                        "cbuffer Params:register(b0){uint4 a;uint4 b;uint4 c;}uint4 main():TEXCOORD0{") +
                        f.body + "}",
                    "vs_5_0")
                    .bytecode;
            std::array<uint32_t, 12> constants{0x80000003, 0x10000000, 0, UINT32_MAX, 13, 0, 33,
                                               31,         0xfedcba98, 7, 0x80000000, 19};
            if (variant == 1)
                constants = {0, 1, 0x80000000, 0x12345678, 0, 32, 1, 0, 5, 6, 7, 8};
            if (variant == 2)
                constants = {2, UINT32_MAX, 1234567, 0x7fffffff, 9, 65, 16, 2, 8, 7, 6, 5};
            if (f.floating) {
                const std::array<float, 12> numbers{1.5f, -2.25f, .5f, -0.f, .25f, 8.f,
                                                    -1.f, 0.f,    2.f, 1.f,  4.f,  8.f};
                std::memcpy(constants.data(), numbers.data(), sizeof constants);
            }
            Inputs inputs;
            for (size_t i = 0; i < 3; ++i)
                inputs.constantBuffers[0].push_back(Value::known(
                    {constants[i * 4], constants[i * 4 + 1], constants[i * 4 + 2], constants[i * 4 + 3]}));
            Program program(shader);
            Lane lane(program, inputs);
            const auto steps = execute(lane);
            QVERIFY(!steps.empty());
            const auto actual = lane.value({2, {0}});
            QCOMPARE(actual.valid, uint8_t(15));
            Device device(vendor);
            const auto reference = device.run(shader, constants);
            for (size_t i = 0; i < 4; ++i) {
                if (f.tolerance) {
                    const auto a = std::bit_cast<float>(actual.bits[i]),
                               b = std::bit_cast<float>(reference[i]);
                    QVERIFY2(std::abs(double(a) - b) <= f.tolerance * std::max(1., std::abs(double(b))),
                             "Native DXBC floating result exceeds the operation tolerance");
                } else
                    QCOMPARE(actual.bits[i], reference[i]);
            }
            // Every recorded change can be applied backwards and forwards exactly,
            // including raw bits and validity, without running the shader again.
            auto values = lane.registers();
            for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                for (auto change = step->changes.rbegin(); change != step->changes.rend(); ++change) {
                    QCOMPARE(values[change->destination], change->after);
                    values[change->destination] = change->before;
                }
            for (const auto &step : steps)
                for (const auto &change : step.changes) {
                    QCOMPARE(values[change.destination], change.before);
                    values[change.destination] = change.after;
                }
            for (const auto &[reg, value] : lane.registers())
                QCOMPARE(values.at(reg), value);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void validityAndRawBits() {
        const auto bytes =
            binary({op(54, {dst(0, 0, 1), literal(0x7fc12345)}), op(54, {dst(2, 0), src(0, 0)}), op(62)});
        Program program(bytes);
        Lane lane(program, {});
        const auto first = lane.step();
        QCOMPARE(first.changes[0].before.valid, uint8_t(0));
        QCOMPARE(first.changes[0].after.valid, uint8_t(1));
        execute(lane);
        QCOMPARE(lane.value({2, {0}}).valid, uint8_t(1));
        QCOMPARE(lane.value({2, {0}}).bits[0], 0x7fc12345u);
        const auto selected = binary({op(54, {dst(0, 0), literal(0x80000000)}),
                                      op(55, {dst(2, 0), literal(1), src(0, 0), src(0, 1)}), op(62)});
        Program selection(selected);
        Lane selectedLane(selection, {});
        execute(selectedLane);
        QCOMPARE(selectedLane.value({2, {0}}),
                 Value::known({0x80000000, 0x80000000, 0x80000000, 0x80000000}));
    }
    void unknownControlAndBudget() {
        Program unknown(binary({op(31 | (1u << 18), {{0x10000a, 0}}), op(21), op(62)}));
        Lane lane(unknown, {});
        const auto before = lane.registers();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
        QCOMPARE(lane.registers(), before);
        Program loop(binary({op(48), op(22), op(62)}));
        Lane bounded(loop, {}, 3);
        bounded.step();
        bounded.step();
        bounded.step();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, bounded.step());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 Program(binary({op(31 | (1u << 18), {literal(1)}), op(62)})));
    }
    void callsAndMultiResults() {
        const auto bytes =
            binary({op(54, {dst(0, 0), literal(3)}), op(4, {{0x10a000, 100}}),
                    op(5 | (1u << 18), {literal(1), {0x10a000, 200}}), op(54, {dst(2, 0), src(0, 0)}), op(62),
                    op(44, {{0x10a000, 100}}), op(30, {dst(0, 0), src(0, 0), literal(10)}), op(62),
                    op(44, {{0x10a000, 200}}), op(4, {{0x10a000, 100}}), op(62)});
        Program program(bytes);
        Lane lane(program, {});
        const auto steps = execute(lane);
        QCOMPARE(lane.value({2, {0}}), Value::known({23, 23, 23, 23}));
        QVERIFY(
            std::any_of(steps.begin(), steps.end(), [](const Step &s) { return s.callstack.size() == 2; }));
        Program multi(binary({op(78, {dst(2, 0, 3), dst(2, 0, 12), literal(11), literal(3)}), op(62)}));
        Lane multiple(multi, {});
        execute(multiple);
        QCOMPARE(multiple.value({2, {0}}), Value::known({3, 3, 2, 2}));
    }
    void floatingEnvironmentAndUnsupported() {
        Program program(binary({op(64, {dst(2, 0), literal(std::bit_cast<uint32_t>(2.5f))}), op(62)}));
        Lane lane(program, {});
        const auto saved = _mm_getcsr();
        const auto changed = (saved & ~0x6000u) | 0x4000u;
        _mm_setcsr(changed);
        const auto step = lane.step();
        const auto after = _mm_getcsr();
        _mm_setcsr(saved);
        QCOMPARE(after, changed);
        QCOMPARE(step.changes[0].after, Value::known({0x40000000, 0x40000000, 0x40000000, 0x40000000}));
        Program derivative(binary({op(11, {dst(2, 0), literal(0)}), op(62)}));
        Lane unsupported(derivative, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, unsupported.step());
        QVERIFY(unsupported.registers().empty());
    }
    void addressesAndAtomicFailure() {
        const Words relative = checkpoint::indexableOperand(0, 0, 15, 0xe4, {}, Words{0x10000a, 0});
        Program unknown(binary({op(105, {{0, 2, 4}}), op(54, {dst(2, 0), relative}), op(62)}));
        Lane unknownLane(unknown, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, unknownLane.step());
        QVERIFY(unknownLane.registers().empty());

        // A multi-result destination is addressed using the pre-instruction
        // register state even when the other result changes its index register.
        const auto indexedDestination = checkpoint::indexableOperand(0, 0, 15, {}, {}, Words{0x10000a, 0});
        const auto indexedSource = checkpoint::indexableOperand(0, 0, 15, 0xe4);
        Program addresses(binary({op(105, {{0, 2, 4}}), op(54, {dst(0, 0), literal(0)}),
                                  op(78, {dst(0, 0), indexedDestination, literal(11), literal(3)}),
                                  op(54, {dst(2, 0), indexedSource}), op(62)}));
        Lane addressed(addresses, {});
        execute(addressed);
        QCOMPARE(addressed.value({0, {0}}), Value::known({3, 3, 3, 3}));
        QCOMPARE(addressed.value({2, {0}}), Value::known({2, 2, 2, 2}));

        Program constants(binary({op(54, {dst(2, 0), {0x00208e46, 0, 3}}), op(62)}));
        Lane unbound(constants, {});
        execute(unbound);
        QCOMPARE(unbound.value({2, {0}}).valid, uint8_t(0));
        Inputs bound;
        bound.constantBuffers[0] = {Value::known({1, 2, 3, 4})};
        Lane outOfBounds(constants, bound);
        execute(outOfBounds);
        QCOMPARE(outOfBounds.value({2, {0}}), Value::known({0, 0, 0, 0}));
        bound.constantBuffers[0][0].valid = 16;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Lane(constants, bound));

        Program invalidSwap(
            binary({op(142, {dst(0, 0, 3), dst(0, 0, 12), literal(1), literal(2), literal(3)}), op(62)}));
        Lane invalid(invalidSwap, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalid.step());
        QVERIFY(invalid.registers().empty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 Program(binary({op(4, {{0x10a000, 100}}), op(62), op(44, {{0x10a000, 100}}),
                                                 op(4, {{0x10a000, 100}}), op(62)})));
    }
    void halfConversionBoundaries() {
        const std::array<std::pair<uint32_t, uint32_t>, 14> cases{{{0, 0},
                                                                   {0x80000000, 0x8000},
                                                                   {0x3f801000, 0x3c00},
                                                                   {0x3f803000, 0x3c01},
                                                                   {0x33000000, 0},
                                                                   {0x33800000, 1},
                                                                   {0x387fc000, 0x3ff},
                                                                   {0x38800000, 0x400},
                                                                   {0x477fe000, 0x7bff},
                                                                   {0x7f7fffff, 0x7bff},
                                                                   {0xff7fffff, 0xfbff},
                                                                   {0x7f800000, 0x7c00},
                                                                   {0xff800000, 0xfc00},
                                                                   {0x7fc12345, 0x7e00}}};
        for (const auto &[input, expected] : cases) {
            Program program(binary({op(130, {dst(2, 0), literal(input)}), op(62)}));
            Lane lane(program, {});
            execute(lane);
            QCOMPARE(lane.value({2, {0}}), Value::known({expected, expected, expected, expected}));
        }
    }
    void instructionCancellationAndValidity() {
        Device device(0);
        int checks = 0;
        Inputs inputs;
        inputs.gpu =
            std::make_shared<GpuOperations>(device.device.Get(), true, [&] { return ++checks == 3; });
        Program program(binary(rawFixture(28, 0, false)));
        Lane cancelled(program, inputs);
        QVERIFY_THROWS_EXCEPTION(OperationCancelled, cancelled.step());
        QCOMPARE(checks, 3);
        QCOMPARE(inputs.gpu->operations(), uint64_t(0));
        QVERIFY(cancelled.registers().empty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, cancelled.step());
        QVERIFY(cancelled.registers().empty());
        inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), true);
        Lane recovered(program, inputs);
        QVERIFY(recovered.step().instructionGpuAssisted);
        QCOMPARE(inputs.gpu->operations(), uint64_t(1));
        QCOMPARE(recovered.value({2, {0}}).valid, uint8_t(15));
        Program unknown(binary({op(132, {dst(2, 0, 3), dst(2, 0, 12), src(0, 0), literal(1)}), op(62)}));
        Lane unknownLane(unknown, inputs);
        QVERIFY(unknownLane.step().instructionGpuAssisted);
        QCOMPARE(unknownLane.value({2, {0}}).valid, uint8_t(0));
        const auto count = inputs.gpu->operations();
        Program overlap(binary({op(132, {dst(2, 0, 3), dst(2, 0, 6), literal(1), literal(2)}), op(62)}));
        Lane invalid(overlap, inputs);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, invalid.step());
        QCOMPARE(inputs.gpu->operations(), count);
        QVERIFY(invalid.registers().empty());
    }
};
QTEST_GUILESS_MAIN(DxbcExecutionTests)
#include "DxbcExecutionTests.moc"
