#include "application/DxbcCheckpointModel.h"
#include "application/DxbcExecution.h"
#include "application/DxbcExecutionTrace.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QtTest>
#include <bit>
#include <chrono>
#include <cmath>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <immintrin.h>
#include <thread>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
void saveArithmeticEvidence(QString name, nlohmann::json report, Bytes native, Bytes oracle = {},
                            const char *environment = "FLORA_DXBC_DOUBLE_EVIDENCE") {
    const auto root = qEnvironmentVariable(environment);
    if (root.isEmpty())
        return;
    QDir directory(root);
    if (!directory.mkpath("."))
        throw std::runtime_error("Cannot create double evidence directory");
    auto write = [&](const QString &suffix, Bytes data) {
        QFile file(directory.filePath(name + suffix));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(reinterpret_cast<const char *>(data.data()), qint64(data.size())) !=
                qint64(data.size()))
            throw std::runtime_error("Cannot preserve double evidence file");
        return QCryptographicHash::hash(
                   QByteArrayView(reinterpret_cast<const char *>(data.data()), qsizetype(data.size())),
                   QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    };
    report["schema"] = 1;
    report["native_sha256"] = write(".native.dxbc", native);
    report["oracle_sha256"] =
        oracle.empty() ? nlohmann::json(nullptr) : nlohmann::json(write(".oracle.dxbc", oracle));
    const auto text = report.dump(2) + "\n";
    write(".json", {reinterpret_cast<const uint8_t *>(text.data()), text.size()});
}
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
    explicit Device(uint32_t vendor, D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0) {
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
        const D3D_FEATURE_LEVEL levels[]{level};
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
std::array<uint32_t, 4> runStructuredFeedback(Device &probe, Bytes shader, Bytes bytes, uint32_t stride) {
    auto device = probe.device.Get();
    auto context = probe.context.Get();
    context->ClearState();
    Com<ID3D11ComputeShader> cs;
    check(device->CreateComputeShader(shader.data(), shader.size(), nullptr, &cs),
          "Create aggregate oracle CS");
    D3D11_BUFFER_DESC desc{UINT(bytes.size()),
                           D3D11_USAGE_DEFAULT,
                           D3D11_BIND_SHADER_RESOURCE,
                           0,
                           D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                           stride};
    D3D11_SUBRESOURCE_DATA initial{bytes.data(), 0, 0};
    Com<ID3D11Buffer> input, output, staging;
    check(device->CreateBuffer(&desc, &initial, &input), "Create aggregate input");
    const std::array<uint32_t, 4> seed{0xcdcdcdcd, 0xcdcdcdcd, 0xcdcdcdcd, 0xcdcdcdcd};
    desc = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            16};
    initial.pSysMem = seed.data();
    check(device->CreateBuffer(&desc, &initial, &output), "Create aggregate output");
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11UnorderedAccessView> uav;
    check(device->CreateShaderResourceView(input.Get(), nullptr, &srv), "Create aggregate SRV");
    check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav), "Create aggregate UAV");
    auto *read = srv.Get();
    auto *write = uav.Get();
    context->CSSetShader(cs.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &read);
    context->CSSetUnorderedAccessViews(0, 1, &write, nullptr);
    context->Dispatch(1, 1, 1);
    context->ClearState();
    desc = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    check(device->CreateBuffer(&desc, nullptr, &staging), "Create aggregate staging");
    context->CopyResource(staging.Get(), output.Get());
    Com<ID3D11Query> completed;
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    check(device->CreateQuery(&query, &completed), "Create aggregate query");
    context->End(completed.Get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
        const auto status = context->GetData(completed.Get(), nullptr, 0, 0);
        check(status, "Wait for aggregate shader");
        if (status == S_OK)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Aggregate GPU deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read aggregate output");
    std::array<uint32_t, 4> result;
    std::memcpy(result.data(), mapped.pData, 16);
    context->Unmap(staging.Get(), 0);
    Com<ID3D11InfoQueue> queue;
    check(probe.device.As(&queue), "Read aggregate validation queue");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T length = 0;
        check(queue->GetMessage(i, nullptr, &length), "Read aggregate validation size");
        std::vector<uint8_t> storage(length);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
        check(queue->GetMessage(i, message, &length), "Read aggregate validation message");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    return result;
}
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
    void pixelAttributeExecutionContracts() {
        try {
            for (uint32_t opcode : {203u, 204u, 205u})
                for (const auto precision : {std::array<uint32_t, 2>{0, 0},
                                             {1, 1},
                                             {2, 2},
                                             {0, 1},
                                             {1, 0},
                                             {0, 2},
                                             {2, 0},
                                             {1, 2},
                                             {2, 1}}) {
                    auto precise = [](Words operand, uint32_t value) {
                        if (value) {
                            operand[0] |= 0x80000000;
                            operand.insert(operand.begin() + 1, 1u | (value << 14));
                        }
                        return operand;
                    };
                    auto attribute = src(1, 0);
                    attribute[0] = (attribute[0] & ~0xff0u) | 0x1b0;
                    attribute = precise(attribute, precision[1]);
                    const auto destination = precise(dst(0, 0, 5), precision[0]);
                    auto location = src(1, 1);
                    if (opcode == 204)
                        location[0] = (location[0] & ~0xffcu) | 8;
                    auto instruction = opcode == 205 ? op(opcode, {destination, attribute})
                                                     : op(opcode, {destination, attribute, location});
                    const std::vector<Words> rows{op(106 | (1 << 16)),
                                                  op(104, {{2}}),
                                                  op(98 | (2 << 11), {dst(1, 0)}),
                                                  op(98 | (1 << 11), {dst(1, 1)}),
                                                  op(101, {dst(2, 0)}),
                                                  op(54, {dst(0, 0), {0x4002, 11, 22, 33, 44}}),
                                                  instruction,
                                                  op(54, {dst(2, 0), src(0, 0)}),
                                                  op(62)};
                    const Program program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50, 0}, rows})}}));
                    for (unsigned form = 0; form < 5; ++form) {
                        if (opcode == 205 && form == 1)
                            continue;
                        unsigned calls = 0;
                        std::array<Inputs, 4> inputs;
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            inputs[lane].registers[{1, {0}}] = {};
                            inputs[lane].registers[{1, {1}}] = {
                                {opcode == 203 ? 0x12345678u + lane : lane, 0xfffffff7, 0, 0},
                                uint8_t(form == 1 ? 0 : 15)};
                            if (form != 2)
                                inputs[lane].evaluateAttribute = [&, lane](const AttributeRequest &request) {
                                    ++calls;
                                    if (request.opcode != opcode || request.inputRegister != 0 ||
                                        request.precision != precision ||
                                        request.location != (opcode == 205 ? std::array<uint32_t, 2>{}
                                                             : opcode == 204
                                                                 ? std::array<uint32_t, 2>{lane, 0}
                                                                 : std::array<uint32_t, 2>{8 + lane, 7}))
                                        throw std::runtime_error(
                                            "Attribute request lost integer location semantics");
                                    return Value{{0x7fc12345, 0x80000000, 0x3f800000, 0xffffffff},
                                                 uint8_t(form == 3   ? 16
                                                         : form == 4 ? 5
                                                                     : 10)};
                                };
                        }
                        PixelQuad quad(program, std::move(inputs), 5);
                        auto run = [&] {
                            while (!quad.finished())
                                quad.step();
                        };
                        if (form == 2 || form == 3) {
                            QVERIFY_EXCEPTION_THROWN(run(), std::runtime_error);
                            continue;
                        }
                        while (!quad.finished())
                            quad.step();
                        QCOMPARE(calls, form == 1 ? 0u : 4u);
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            const auto value = quad.lane(lane).value({2, {0}});
                            QCOMPARE(value.valid, uint8_t(form == 1 || form == 4 ? 10 : 15));
                            QCOMPARE(value.bits, (std::array<uint32_t, 4>{form == 1 ? 0u : 0xffffffff, 22,
                                                                          form == 1 ? 0u : 0x80000000, 44}));
                        }
                    }
                    Lane standalone(program, Inputs{});
                    QVERIFY_EXCEPTION_THROWN(execute(standalone), std::runtime_error);
                }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void maskedSad_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("form");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                for (uint32_t form = 0; form < 4; ++form)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(stage).arg(form)))
                        << vendor << stage << form;
    }
    void maskedSad() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, form);
        try {
            Device device(vendor);
            D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
            check(device.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)),
                  "Read MSAD test support");
            std::vector<uint8_t> lastCode, lastOracle;
            for (uint32_t pattern = 0; pattern < 4; ++pattern) {
                const std::array<uint32_t, 4> reference{0, 0x010000ff, 0xffffffff, 0x807f0100};
                const std::array<uint32_t, 4> source{0xffffffff, 0xff010000, 0, 0x7f8000ff};
                const std::array<uint32_t, 4> accum{17, 23, 100,
                                                    pattern == 0 ? 100u
                                                                 : UINT32_MAX - (pattern == 1   ? 3u
                                                                                 : pattern == 2 ? 2u
                                                                                                : 0u)};
                const std::array<uint32_t, 4> sentinel{19, 31, 47, 59};
                const uint32_t mask = form == 1 ? 5 : 15;
                Inputs inputs;
                inputs.constantBuffers[0] = {Value::known(reference), Value::known(source),
                                             Value::known(accum)};
                if (form == 3) {
                    inputs.constantBuffers[0][0].valid = 11;
                    inputs.constantBuffers[0][1].valid = 14;
                }
                Words a{0x208e46, 0, 0}, b{0x208e46, 0, 1}, c{0x208e46, 0, 2};
                if (form == 1) {
                    a[0] = (a[0] & ~0xff0u) | 0x1b0u;
                    b[0] = (b[0] & ~0xff0u) | 0x1b0u;
                    c[0] = (c[0] & ~0xff0u) | 0x1b0u;
                } else if (form == 2) {
                    a = vectorLiteral(reference);
                    b = vectorLiteral(source);
                    c = vectorLiteral(accum);
                }
                const auto operation = op(213, {dst(0, 0, mask), a, b, c});
                std::vector<Words> rows{op(89, {{0x208e46, 0, 3}}), op(104, {{1}})};
                if (stage == 5)
                    rows.push_back(op(155, {{1, 1, 1}}));
                rows.push_back(op(54, {dst(0, 0), vectorLiteral(sentinel)}));
                rows.push_back(operation);
                rows.push_back(op(62));
                lastCode = makeDxbc({{0x58454853, writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows})}});
                const Program program(lastCode);
                Lane lane(program, inputs);
                lane.step();
                const auto step = lane.step();
                QVERIFY(!step.instructionGpuAssisted);
                QCOMPARE(step.changes.size(), size_t(1));
                QCOMPARE(step.changes[0].before, Value::known(sentinel));
                const auto actual = lane.value({0, {0}});
                QCOMPARE(step.changes[0].after, actual);
                uint8_t valid = pattern < 2 ? 15 : 7;
                if (form == 1)
                    valid = pattern < 2 ? 15 : 14;
                if (form == 3)
                    valid &= 11;
                QCOMPARE(actual.valid, valid);
                if (form == 1) {
                    QCOMPARE(actual.bits[1], sentinel[1]);
                    QCOMPARE(actual.bits[3], sentinel[3]);
                } else {
                    QCOMPARE(actual.bits[0], 17u);
                    QCOMPARE(actual.bits[1], 532u);
                    if (form != 3)
                        QCOMPARE(actual.bits[2], 1120u);
                }
                nlohmann::json row{{"pattern", pattern},
                                   {"reference", reference},
                                   {"source", source},
                                   {"accum", accum},
                                   {"input_valid",
                                    {inputs.constantBuffers[0][0].valid, inputs.constantBuffers[0][1].valid,
                                     inputs.constantBuffers[0][2].valid}},
                                   {"mask", mask},
                                   {"actual", actual.bits},
                                   {"valid", actual.valid},
                                   {"gpu", nullptr},
                                   {"gpu_supported", bool(options.SAD4ShaderInstructions)}};
                if (options.SAD4ShaderInstructions) {
                    lastOracle =
                        gpuBinary({op(89, {{0x208e46, 0, 3}}), op(54, {dst(0, 0), vectorLiteral(sentinel)}),
                                   operation, op(54, {dst(2, 0), src(0, 0)}), op(62)});
                    std::vector<uint32_t> constants;
                    for (const auto &value : inputs.constantBuffers[0])
                        constants.insert(constants.end(), value.bits.begin(), value.bits.end());
                    const auto gpu = device.run(lastOracle, constants);
                    row["gpu"] = gpu;
                    for (unsigned component = 0; component < 4; ++component)
                        if (valid & (1u << component))
                            QCOMPARE(actual.bits[component], gpu[component]);
                }
                row.update({{"kind", "native_msad"},
                            {"vendor", vendor},
                            {"stage", stage},
                            {"form", form},
                            {"oracle_stage", "vs"}});
                saveArithmeticEvidence(QString("msad-") + QTest::currentDataTag() + "-" +
                                           QString::number(pattern),
                                       row, lastCode, lastOracle, "FLORA_DXBC_MSAD_EVIDENCE");
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void compiledMaskedSad_data() {
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            QTest::newRow(qPrintable(QString::number(vendor))) << vendor;
    }
    void compiledMaskedSad() {
        QFETCH(uint32_t, vendor);
        try {
            Device device(vendor);
            const auto shader = compileHlsl("cbuffer Params:register(b0){uint4 a,b,c;}"
                                            "uint4 main():TEXCOORD0{return msad4(a.x,b.xy,c);}",
                                            "vs_5_0")
                                    .bytecode;
            const Program program(shader);
            QVERIFY(std::any_of(program.instructions().begin(), program.instructions().end(),
                                [](const auto &instruction) { return instruction.opcode == 213; }));
            Inputs inputs;
            inputs.constantBuffers[0] = {Value::known({0xa100b2c3, 0, 0, 0}),
                                         Value::known({0xd7b0c372, 0x4f57c2a7, 0, 0}),
                                         Value::known({1, 2, 3, 4})};
            Lane lane(program, inputs);
            execute(lane);
            const auto actual = lane.value({2, {0}});
            QCOMPARE(actual.valid, uint8_t(15));
            std::vector<uint32_t> constants;
            for (const auto &value : inputs.constantBuffers[0])
                constants.insert(constants.end(), value.bits.begin(), value.bits.end());
            D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
            check(device.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)),
                  "Read compiled MSAD support");
            nlohmann::json gpu = nullptr;
            if (options.SAD4ShaderInstructions) {
                const auto expected = device.run(shader, constants);
                QCOMPARE(actual.bits, expected);
                gpu = expected;
            }
            saveArithmeticEvidence(QString("compiled-msad-") + QTest::currentDataTag(),
                                   {{"kind", "compiled_native_msad"},
                                    {"vendor", vendor},
                                    {"constants", constants},
                                    {"actual", actual.bits},
                                    {"valid", actual.valid},
                                    {"gpu", gpu},
                                    {"gpu_supported", bool(options.SAD4ShaderInstructions)}},
                                   shader, {}, "FLORA_DXBC_MSAD_EVIDENCE");
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void compiledFeedbackAggregation_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("fields");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t fields : {2u, 3u, 4u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(fields))) << vendor << fields;
    }
    void compiledFeedbackAggregation() {
        try {
            const bool partial = QString::fromLatin1(QTest::currentTestFunction()) == "nullBufferData";
            QFETCH(uint32_t, vendor);
            QFETCH(uint32_t, fields);
            std::string source = "struct Element{";
            for (uint32_t i = 0; i < fields; ++i)
                source += "uint4 v" + std::to_string(i) + ";";
            source += "};StructuredBuffer<Element> inputData:register(t0);"
                      "RWStructuredBuffer<uint4> outputData:register(u0);"
                      "[numthreads(1,1,1)]void main(){uint status;Element value=inputData.Load(1,status);"
                      "uint4 sum=value.v0;";
            for (uint32_t i = 1; i < (partial ? 1u : fields); ++i)
                source += "sum+=value.v" + std::to_string(i) + ";";
            source += "outputData[0]=uint4(sum.xyz,CheckAccessFullyMapped(status));}";
            const auto original = compileHlsl(source, "cs_5_0").bytecode;
            auto parts = readDxbcParts(original);
            std::vector<uint8_t> rewritten;
            uint32_t checks = 0, rewrittenMerges = 0;
            // This compiler emits per-access checks and Boolean AND. Exercise
            // the specification's raw OR form by retaining feedback tokens
            // until a single final check, with the original as a GPU control.
            for (auto &[tag, data] : parts) {
                if (tag != 0x58454853 && tag != 0x52444853)
                    continue;
                auto code = readDxbcProgram(data);
                std::vector<Words> rows;
                for (auto row : code.instructions) {
                    const auto opcode = row[0] & 2047;
                    if (opcode == 234) {
                        row[0] = (row[0] & ~2047u) | 54u;
                        ++checks;
                    }
                    const bool merge = opcode == 1 && row.size() == 7 && ((row[3] >> 12) & 255) == 0 &&
                                       ((row[5] >> 12) & 255) == 0;
                    if (merge) {
                        row[0] = (row[0] & ~2047u) | 60u;
                        ++rewrittenMerges;
                    }
                    rows.push_back(row);
                    if (merge && rewrittenMerges == fields - 1) {
                        const auto mask = (row[1] >> 4) & 15;
                        QVERIFY(std::has_single_bit(mask));
                        rows.push_back(
                            op(234, {Words{row[1], row[2]},
                                     Words{0x10000a | (uint32_t(std::countr_zero(mask)) << 4), row[2]}}));
                    }
                }
                code.instructions = std::move(rows);
                rewritten = writeDxbcProgram(code);
                data = rewritten;
            }
            QCOMPARE(checks, fields);
            QCOMPARE(rewrittenMerges, fields - 1);
            const auto shader = makeDxbc(parts);
            const Program program(shader);
            uint32_t loads = 0, merges = 0, nullLoads = 0;
            for (const auto &instruction : program.instructions()) {
                loads += instruction.opcode == 227;
                merges += instruction.opcode == 60;
                if (instruction.opcode == 227) {
                    size_t first = 1;
                    while (instruction.words.at(first - 1) & 0x80000000u)
                        ++first;
                    nullLoads += instruction.words.at(first) == 0xd000;
                }
            }
            QCOMPARE(loads, fields);
            QCOMPARE(merges, fields - 1);
            QCOMPARE(nullLoads, partial ? fields - 1 : 0u);
            const auto stride = fields * 16;
            std::vector<uint8_t> bytes(stride * 2);
            for (uint32_t i = 0; i < bytes.size() / 4; ++i) {
                const uint32_t value = 100 + i;
                std::memcpy(bytes.data() + i * 4, &value, 4);
            }
            std::array<uint32_t, 4> expected{0, 0, 0, 1};
            for (unsigned c = 0; c < 3; ++c)
                for (uint32_t i = 0; i < (partial ? 1u : fields); ++i)
                    expected[c] += 100 + fields * 4 + i * 4 + c;
            Device device(vendor, D3D_FEATURE_LEVEL_11_1);
            const auto control = runStructuredFeedback(device, original, bytes, stride);
            const auto gpu = runStructuredFeedback(device, shader, bytes, stride);
            QCOMPARE(control, expected);
            QCOMPARE(gpu, expected);
            nlohmann::json observations = nlohmann::json::array();
            for (bool knownMapping : {false, true}) {
                BufferView input{1, 0, bytes.size(), stride};
                input.nonTiled = knownMapping;
                Inputs inputs;
                inputs.memory = std::make_shared<Memory>(
                    std::map<uint64_t, Buffer>{{1, Buffer::known(bytes)},
                                               {2, Buffer::known(std::vector<uint8_t>(16))}},
                    std::map<Register, BufferView>{{{7, {0}}, input}, {{30, {0}}, {2, 0, 16, 16}}});
                Lane lane(program, inputs);
                const auto steps = execute(lane);
                for (const auto &step : steps)
                    for (const auto &change : step.changes) {
                        QVERIFY(change.after.wellFormed());
                        QVERIFY(change.destination.kind != 13);
                    }
                const auto &output = inputs.memory->buffers().at(2);
                std::array<uint32_t, 4> words;
                std::memcpy(words.data(), output.bytes.data(), 16);
                for (unsigned c = 0; c < 3; ++c)
                    QCOMPARE(words[c], expected[c]);
                if (knownMapping)
                    QCOMPARE(words, gpu);
                for (unsigned i = 0; i < 16; ++i)
                    QCOMPARE(output.valid[i], uint8_t(i < 12 || knownMapping));
                QCOMPARE(inputs.memory->buffers().at(1).bytes, bytes);
                observations.push_back({{"known_mapping", knownMapping},
                                        {"words", words},
                                        {"validity", output.valid},
                                        {"steps", steps.size()}});
            }
            const auto root = qEnvironmentVariable(partial ? "FLORA_FEEDBACK_NULL_BUFFER_EVIDENCE"
                                                           : "FLORA_FEEDBACK_AGGREGATE_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag())));
                QVERIFY(directory.mkpath("."));
                QFile binaryFile(directory.filePath("shader.dxbc"));
                QVERIFY(binaryFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                QCOMPARE(binaryFile.write(reinterpret_cast<const char *>(shader.data()), shader.size()),
                         qint64(shader.size()));
                QFile originalFile(directory.filePath("original.dxbc"));
                QVERIFY(originalFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                QCOMPARE(originalFile.write(reinterpret_cast<const char *>(original.data()), original.size()),
                         qint64(original.size()));
                QFile reportFile(directory.filePath("report.json"));
                QVERIFY(reportFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                const nlohmann::json report{{"fields", fields},
                                            {"vendor", vendor},
                                            {"loads", loads},
                                            {"merges", merges},
                                            {"null_loads", nullLoads},
                                            {"shader_sha256", sha256(shader)},
                                            {"original_sha256", sha256(original)},
                                            {"control", control},
                                            {"gpu", gpu},
                                            {"expected", expected},
                                            {"native", observations}};
                const auto encoded = report.dump(2);
                QCOMPARE(reportFile.write(encoded.data(), encoded.size()), qint64(encoded.size()));
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void nullBufferData_data() { compiledFeedbackAggregation_data(); }
    void nullBufferData() { compiledFeedbackAggregation(); }
    void nullRawFeedback() {
        for (bool known : {false, true})
            for (bool discardStatus : {false, true}) {
                BufferView view{1, 0, 16};
                view.nonTiled = known;
                Inputs inputs;
                inputs.memory = std::make_shared<Memory>(
                    std::map<uint64_t, Buffer>{{1, Buffer::known(std::vector<uint8_t>(16))}},
                    std::map<Register, BufferView>{{{7, {0}}, view}});
                auto rows =
                    std::vector<Words>{op(161, {dst(7, 0)}),
                                       op(226, {Words{0xd000}, discardStatus ? Words{0xd000} : dst(0, 0, 1),
                                                literal(0), src(7, 0)})};
                if (!discardStatus)
                    rows.push_back(op(234, {dst(2, 0, 1), Words{0x10000a, 0}}));
                rows.push_back(op(62));
                const Program program(binary(rows));
                Lane lane(program, inputs);
                const auto steps = execute(lane);
                for (const auto &step : steps)
                    for (const auto &change : step.changes)
                        QVERIFY(change.destination.kind != 13);
                if (discardStatus)
                    QVERIFY(lane.registers().empty());
                else {
                    QCOMPARE(lane.value({2, {0}}).valid, uint8_t(known));
                    QCOMPARE(lane.value({2, {0}}).bits[0], known ? UINT32_MAX : 0u);
                }
                for (const auto &invalid :
                     std::vector<Words>{{0xd001}, {0xd002}, {0xd010}, {0x10d000, 0}, {0x8000d000, 0x41}}) {
                    const Program malformed(
                        binary({op(161, {dst(7, 0)}), op(226, {invalid, dst(0, 0, 1), literal(0), src(7, 0)}),
                                op(62)}));
                    Lane rejected(malformed, inputs);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(rejected));
                    QVERIFY(rejected.registers().empty());
                }
                for (const auto &invalid : std::vector<Words>{{0xd000}, {0xd0f2}}) {
                    const Program ordinary(
                        binary({op(161, {dst(7, 0)}), op(165, {invalid, literal(0), src(7, 0)}), op(62)}));
                    Lane rejected(ordinary, inputs);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(rejected));
                    QVERIFY(rejected.registers().empty());
                }
            }
    }
    void feedbackStatusAggregation() {
        const auto inputsFor = [](bool leftKnown, bool rightKnown) {
            Inputs inputs;
            BufferView left{1, 0, 16}, right{2, 0, 16};
            left.nonTiled = leftKnown;
            right.nonTiled = rightKnown;
            inputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(std::vector<uint8_t>(16))},
                                           {2, Buffer::known(std::vector<uint8_t>(16))}},
                std::map<Register, BufferView>{{{7, {0}}, left}, {{7, {1}}, right}});
            return inputs;
        };
        const auto prefix = [] {
            return std::vector<Words>{op(161, {dst(7, 0)}), op(161, {dst(7, 1)}),
                                      op(226, {dst(0, 3), dst(0, 1, 1), literal(0), src(7, 0)}),
                                      op(226, {dst(0, 3), dst(0, 2, 1), literal(0), src(7, 1)}),
                                      op(54, {dst(0, 0), Words{0x100006, 1}})};
        };
        for (bool leftKnown : {false, true})
            for (bool rightKnown : {false, true})
                for (uint32_t mask : {5u, 15u}) {
                    auto rows = prefix();
                    rows.push_back(op(60, {dst(0, 0, mask), src(0, 0), Words{0x100006, 2}}));
                    rows.push_back(op(234, {dst(2, 0, 1), Words{0x10000a, 0}}));
                    rows.push_back(op(62));
                    const Program program(binary(rows));
                    Lane lane(program, inputsFor(leftKnown, rightKnown));
                    const auto steps = execute(lane);
                    const auto result = lane.value({0, {0}});
                    const auto combined =
                        leftKnown && rightKnown ? MappingStatus::Mapped : MappingStatus::Unknown;
                    QVERIFY(result.wellFormed());
                    QCOMPARE(result.valid, uint8_t(0));
                    for (unsigned c = 0; c < 4; ++c) {
                        QCOMPARE(result.bits[c], 0u);
                        QCOMPARE(result.mapping[c], (mask & (1u << c)) ? combined
                                                    : leftKnown        ? MappingStatus::Mapped
                                                                       : MappingStatus::Unknown);
                    }
                    const auto checked = lane.value({2, {0}});
                    QCOMPARE(checked.valid, uint8_t(leftKnown && rightKnown));
                    QCOMPARE(checked.bits[0], leftKnown && rightKnown ? UINT32_MAX : 0u);
                    auto restored = lane.registers();
                    for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                        for (auto change = step->changes.rbegin(); change != step->changes.rend(); ++change)
                            restored[change->destination] = change->before;
                    QCOMPARE(restored.at({0, {0}}), Value{});
                }
        for (bool known : {false, true})
            for (bool reverse : {false, true})
                for (unsigned markerKind : {0u, 1u, 2u}) {
                    auto rows = prefix();
                    const auto marker = markerKind == 2 ? src(1, 0) : literal(markerKind == 1 ? 0u : 17u);
                    const auto status = src(0, 0);
                    rows.push_back(op(60, {dst(0, 0), reverse ? marker : status, reverse ? status : marker}));
                    rows.push_back(op(62));
                    const Program program(binary(rows));
                    Lane lane(program, inputsFor(known, known));
                    execute(lane);
                    const auto result = lane.value({0, {0}});
                    QVERIFY(result.wellFormed());
                    QCOMPARE(result.valid, uint8_t(0));
                    QCOMPARE(result.mapping[0], markerKind != 1 ? MappingStatus::NotStatus
                                                : known         ? MappingStatus::Mapped
                                                                : MappingStatus::Unknown);
                }
    }
    void feedbackStatusCopies() {
        const Words token{0x10000a, 2}, replicated{0x100006, 1};
        for (bool nonTiled : {false, true})
            for (bool knownCondition : {false, true})
                for (uint32_t operation : {54u, 55u, 142u}) {
                    const auto condition = knownCondition ? literal(1) : src(1, 0);
                    std::vector<Words> rows{op(161, {dst(7, 0)}),
                                            op(226, {dst(0, 0), dst(0, 1, 1), literal(0), src(7, 0)})};
                    if (operation == 54)
                        rows.push_back(op(operation, {dst(0, 2), replicated}));
                    else if (operation == 55)
                        rows.push_back(op(operation, {dst(0, 2), condition, replicated, replicated}));
                    else
                        rows.push_back(
                            op(operation, {dst(0, 2), dst(0, 3), condition, replicated, replicated}));
                    rows.push_back(op(30, {dst(0, 2, 6), literal(7), literal(9)}));
                    rows.push_back(op(234, {dst(2, 0, 1), token}));
                    rows.push_back(op(62));
                    BufferView view{1, 0, 16};
                    view.nonTiled = nonTiled;
                    Inputs inputs;
                    inputs.memory = std::make_shared<Memory>(
                        std::map<uint64_t, Buffer>{{1, Buffer::known(std::vector<uint8_t>(16))}},
                        std::map<Register, BufferView>{{{7, {0}}, view}});
                    const Program program(binary(rows));
                    Lane lane(program, inputs);
                    execute(lane);
                    const auto &copied = lane.registers().at({0, {2}});
                    QCOMPARE(copied.mapping[0], nonTiled ? MappingStatus::Mapped : MappingStatus::Unknown);
                    QCOMPARE(copied.mapping[3], copied.mapping[0]);
                    QCOMPARE(copied.mapping[1], MappingStatus::NotStatus);
                    QCOMPARE(copied.mapping[2], MappingStatus::NotStatus);
                    QCOMPARE(copied.valid, uint8_t(6));
                    const auto &checked = lane.registers().at({2, {0}});
                    QCOMPARE(checked.valid, uint8_t(nonTiled));
                    QCOMPARE(checked.bits[0], nonTiled ? UINT32_MAX : 0u);
                    // Reusing the token register for ordinary data must invalidate its provenance.
                    rows.insert(rows.end() - 2, op(54, {dst(0, 2, 1), literal(UINT32_MAX)}));
                    const Program overwritten(binary(rows));
                    Lane invalid(overwritten, inputs);
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(invalid));
                    auto narrowed = dst(0, 2);
                    narrowed[0] |= 0x80000000u;
                    narrowed.insert(narrowed.begin() + 1, 1u | (5u << 14));
                    rows[2] = op(54, {narrowed, replicated});
                    const Program precision(binary(rows));
                    Lane unsafe(precision, inputs);
                    try {
                        execute(unsafe);
                        QFAIL("Opaque status was accepted by minimum-precision arithmetic");
                    } catch (const std::runtime_error &error) {
                        QCOMPARE(
                            QString::fromUtf8(error.what()),
                            QString("Minimum-precision operations cannot consume opaque mapping status"));
                    }
                }
    }
    void feedbackStatus_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("uav");
        QTest::addColumn<bool>("nonTiled");
        QTest::addColumn<uint32_t>("vendor");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (int type = 0; type != 3; ++type)
                for (bool uav : {false, true})
                    for (bool nonTiled : {false, true})
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(type).arg(uav).arg(nonTiled)))
                            << type << uav << nonTiled << vendor;
    }
    void feedbackStatus() {
        try {
            QFETCH(int, type);
            QFETCH(bool, uav);
            QFETCH(bool, nonTiled);
            QFETCH(uint32_t, vendor);
            const std::string types[]{"Buffer<uint4>", "ByteAddressBuffer", "StructuredBuffer<uint4>"};
            const auto source = std::string(uav ? "RW" : "") + types[type] + " inputData:register(" +
                                (uav ? "u0" : "t0") +
                                ");RWStructuredBuffer<uint4> outputData:register(u1);"
                                "[numthreads(1,1,1)]void main(){uint status;uint4 value=inputData.Load(" +
                                (type == 1 ? "8" : "2") +
                                ",status);outputData[0]=value;"
                                "outputData[1]=uint4(CheckAccessFullyMapped(status),99,0x0011e000,123);}";
            const auto bytecode = compileHlsl(source, "cs_5_0").bytecode;
            const Program program(bytecode);
            std::vector<uint8_t> bytes(128);
            for (uint32_t i = 0; i < 32; ++i) {
                const uint32_t value = 100 + i;
                std::memcpy(bytes.data() + 4 * i, &value, 4);
            }
            BufferView input{1, 0, 128, type == 2 ? 16u : 0u};
            input.format = type == 0 ? DXGI_FORMAT_R32_UINT : 0;
            input.nonTiled = nonTiled;
            Inputs inputs;
            Device device(vendor);
            inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            inputs.memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{{1, Buffer::known(bytes)},
                                           {2, Buffer::known(std::vector<uint8_t>(32))}},
                std::map<Register, BufferView>{{{uav ? 30u : 7u, {0}}, input}, {{30, {1}}, {2, 0, 32, 16}}},
                256 * 1024 * 1024, 16 * 1024 * 1024, inputs.gpu);
            Lane lane(program, inputs);
            bool observed = false;
            for (const auto &step : execute(lane))
                for (const auto &change : step.changes) {
                    QVERIFY(change.before.wellFormed());
                    QVERIFY(change.after.wellFormed());
                    for (unsigned c = 0; c < 4; ++c)
                        if (change.after.mapping[c] != MappingStatus::NotStatus) {
                            observed = true;
                            QCOMPARE(change.after.mapping[c],
                                     nonTiled ? MappingStatus::Mapped : MappingStatus::Unknown);
                            QVERIFY(!(change.after.valid & (1u << c)));
                            QCOMPARE(change.after.bits[c], 0u);
                        }
                }
            QVERIFY(observed);
            const auto &output = inputs.memory->buffers().at(2);
            std::array<uint32_t, 8> words{};
            std::memcpy(words.data(), output.bytes.data(), output.bytes.size());
            const std::array<uint32_t, 4> expected = type == 0 ? std::array<uint32_t, 4>{102, 0, 0, 1}
                                                     : type == 1
                                                         ? std::array<uint32_t, 4>{102, 102, 102, 102}
                                                         : std::array<uint32_t, 4>{108, 109, 110, 111};
            for (unsigned i = 0; i < 4; ++i)
                QCOMPARE(words[i], expected[i]);
            if (nonTiled)
                QCOMPARE(words[4], 1u);
            for (unsigned i = 0; i < 32; ++i)
                QCOMPARE(output.valid[i], uint8_t(i >= 16 && i < 20 ? nonTiled : true));
            QCOMPARE(words[5], 99u);
            QCOMPARE(words[6], 0x0011e000u);
            QCOMPARE(words[7], 123u);
            QCOMPARE(inputs.memory->buffers().at(1).bytes, bytes);
            const auto root = qEnvironmentVariable("FLORA_FEEDBACK_NATIVE_EVIDENCE");
            if (!root.isEmpty()) {
                QDir directory(QDir(root).filePath(QString::fromLatin1(QTest::currentDataTag())));
                QVERIFY(directory.mkpath("."));
                InputSnapshot snapshot;
                snapshot.inputs = inputs;
                snapshot.stage = 5;
                snapshot.event = 1;
                snapshot.shader = 2;
                snapshot.bytecode = bytecode;
                snapshot.frameSha256 = std::string(64, '0');
                snapshot.resourceObservation = "synthetic_feedback_fixture";
                const auto gpu = inputs.gpu->identity();
                snapshot.adapterLuid = uint64_t(gpu.at("luid_low").get<uint32_t>()) |
                                       (uint64_t(gpu.at("luid_high").get<uint32_t>()) << 32);
                snapshot.adapterVendor = gpu.at("vendor_id");
                snapshot.adapterDevice = gpu.at("device_id");
                const auto buffers = [&] {
                    nlohmann::json result = nlohmann::json::array();
                    for (const auto &[id, buffer] : inputs.memory->buffers())
                        result.push_back({{"resource", id},
                                          {"sha256", sha256(buffer.bytes)},
                                          {"validity_sha256", sha256(buffer.valid)}});
                    return result;
                };
                nlohmann::json report{{"scope", "synthetic_feedback_fixture"},
                                      {"event", snapshot.event},
                                      {"shader", snapshot.shader},
                                      {"frame_sha256", snapshot.frameSha256},
                                      {"shader_sha256", sha256(bytecode)},
                                      {"adapter_luid", snapshot.adapterLuid},
                                      {"buffers", buffers()},
                                      {"textures", nlohmann::json::array()},
                                      {"non_tiled", nonTiled},
                                      {"type", type},
                                      {"uav", uav},
                                      {"words", words},
                                      {"validity", output.valid}};
                report["trace"] = exportGroupExecutionTrace(directory.filePath("trace.jsonl"), snapshot);
                report["trace"]["buffers_after"] = buffers();
                report["trace"]["textures_after"] = nlohmann::json::array();
                QFile shader(directory.filePath("shader.dxbc"));
                QVERIFY(shader.open(QIODevice::WriteOnly));
                QCOMPARE(shader.write(reinterpret_cast<const char *>(bytecode.data()), bytecode.size()),
                         qint64(bytecode.size()));
                shader.close();
                QFile result(directory.filePath("report.json"));
                QVERIFY(result.open(QIODevice::WriteOnly));
                const auto text = QByteArray::fromStdString(report.dump(2) + "\n");
                QCOMPARE(result.write(text), text.size());
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void doubleInstructions_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<uint32_t>("form");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                for (uint32_t opcode : {191u, 192u, 193u, 194u, 195u, 196u, 197u, 198u, 199u, 200u, 201u,
                                        202u, 210u, 211u, 212u, 214u, 215u, 216u, 217u})
                    for (uint32_t form : {0u, 1u, 2u, 3u})
                        QTest::newRow(
                            qPrintable(QString("%1-%2-%3-%4").arg(vendor).arg(stage).arg(opcode).arg(form)))
                            << vendor << stage << opcode << form;
    }
    void doubleInstructions() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, opcode);
        QFETCH(uint32_t, form);
        try {
            const auto pair = [](double x, double y) {
                const auto a = std::bit_cast<uint64_t>(x), b = std::bit_cast<uint64_t>(y);
                return std::array<uint32_t, 4>{uint32_t(a), uint32_t(a >> 32), uint32_t(b),
                                               uint32_t(b >> 32)};
            };
            const bool scalarResult =
                (opcode >= 195 && opcode <= 198) || opcode == 201 || opcode == 214 || opcode == 215;
            const bool scalarInput = opcode == 202 || opcode == 216 || opcode == 217;
            const uint32_t mask = scalarResult ? (form == 1 ? 8u : 5u) : (form == 1 ? 12u : 15u);
            const auto flags = op(106 | (1u << 12) | (opcode >= 210 ? 1u << 17 : 0u));
            std::vector<std::array<uint32_t, 4>> values{pair(1.5, -2.25), pair(0.5, 4.0), pair(-1.0, 2.0)};
            if (scalarInput)
                values[0] = opcode == 202
                                ? std::array<uint32_t, 4>{0x3fc00000, 0xc0100000, 0x40800000, 0x3f000000}
                                : std::array<uint32_t, 4>{3, 0xfffffffd, 5, 9};
            if (opcode == 200)
                values[0] = {1, 0, 0, 1};
            const size_t count =
                opcode == 200 || opcode == 211 ? 3 : (opcode <= 198 || opcode == 210 ? 2 : 1);
            Words operation{opcode};
            const auto destination = dst(0, 0, mask);
            operation.insert(operation.end(), destination.begin(), destination.end());
            Inputs inputs;
            uint8_t expectedValid = uint8_t(mask);
            for (size_t i = 0; i < count; ++i) {
                const bool scalar = scalarInput || (opcode == 200 && i == 0);
                Words argument;
                if (form == 2)
                    argument = {scalar ? 0x4002u : 0x5002u, values[i][0], values[i][1], values[i][2],
                                values[i][3]};
                else {
                    argument = {0x208e46, 0, uint32_t(i)};
                    if (form == 1 && !scalar) {
                        argument[0] = (argument[0] & ~0xff0u) | 0x4e0u;
                        argument[0] |= 0x80000000u;
                        argument.insert(argument.begin() + 1, 1u | (3u << 6));
                    }
                }
                operation.insert(operation.end(), argument.begin(), argument.end());
                auto value = Value::known(values[i]);
                if (form == 3 && i == count - 1) {
                    // Only the second result is uncertain; an unselected DMOVC branch stays irrelevant.
                    value.valid = scalar ? 13 : 7;
                    expectedValid &= uint8_t(scalarResult ? ~4u : ~12u);
                }
                inputs.constantBuffers[0].push_back(value);
            }
            operation[0] |= uint32_t(operation.size()) << 24;
            const std::array<uint32_t, 4> sentinel{0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00};
            expectedValid |= uint8_t(15u & ~mask);
            std::vector<Words> rows{flags, op(89, {{0x208e46, 0, uint32_t(count)}}), op(104, {{1}})};
            if (stage == 5)
                rows.push_back(op(155, {{1, 1, 1}}));
            rows.push_back(op(54, {dst(0, 0), vectorLiteral(sentinel)}));
            rows.push_back(operation);
            rows.push_back(op(62));
            const auto bytes = writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows});
            const auto executable = makeDxbc({{0x58454853, bytes}});
            Program program(executable);
            Device device(vendor);
            D3D11_FEATURE_DATA_DOUBLES doubles{};
            D3D11_FEATURE_DATA_D3D11_OPTIONS extended{};
            check(device.device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles)),
                  "Read double test capability");
            check(
                device.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &extended, sizeof(extended)),
                "Read extended double test capability");
            inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            Lane lane(program, inputs);
            lane.step();
            const auto initialized = lane.registers();
            nlohmann::json report{{"scope", "double_instruction_operand_semantics"},
                                  {"vendor", vendor},
                                  {"stage", stage},
                                  {"opcode", opcode},
                                  {"form", form},
                                  {"mask", mask},
                                  {"basic_support", bool(doubles.DoublePrecisionFloatShaderOps)},
                                  {"extended_support", bool(extended.ExtendedDoublesShaderInstructions)},
                                  {"identity", inputs.gpu->identity()}};
            if (!doubles.DoublePrecisionFloatShaderOps ||
                (opcode >= 210 && !extended.ExtendedDoublesShaderInstructions)) {
                try {
                    lane.step();
                    QFAIL("Unsupported double instruction unexpectedly executed");
                } catch (const std::runtime_error &error) {
                    const std::string expected =
                        doubles.DoublePrecisionFloatShaderOps
                            ? "Selected adapter does not support extended double shader instructions"
                            : "Selected adapter does not support double shader instructions";
                    QCOMPARE(std::string(error.what()), expected);
                    report["error"] = error.what();
                }
                QCOMPARE(lane.registers(), initialized);
                QCOMPARE(inputs.gpu->operations(), uint64_t(0));
                report["outcome"] = "hardware_unsupported";
                report["gpu_operations"] = 0;
                report["state_unchanged"] = true;
                saveArithmeticEvidence(QString("raw-") + QTest::currentDataTag(), report, executable);
                qInfo() << "Double hardware unsupported: explicit rejection verified"
                        << doubles.DoublePrecisionFloatShaderOps
                        << extended.ExtendedDoublesShaderInstructions;
                return;
            }
            const auto step = lane.step();
            QVERIFY(step.instructionGpuAssisted);
            QCOMPARE(step.changes.size(), size_t(1));
            QCOMPARE(step.changes[0].before, Value::known(sentinel));
            const auto actual = lane.value({0, {0}});
            QCOMPARE(step.changes[0].after, actual);
            QCOMPARE(actual.valid, expectedValid);
            QCOMPARE(inputs.gpu->operations(), uint64_t(1));
            // Execute the original operand encoding directly in an independent whole VS.
            // This oracle does not call the instruction rewriting or assistance path.
            const auto shader = gpuBinary({flags, op(89, {{0x208e46, 0, uint32_t(count)}}),
                                           op(54, {dst(0, 0), vectorLiteral(sentinel)}), operation,
                                           op(54, {dst(2, 0), src(0, 0)}), op(62)});
            std::vector<uint32_t> constants;
            for (size_t i = 0; i < count; ++i)
                constants.insert(constants.end(), values[i].begin(), values[i].end());
            const auto expected = device.run(shader, constants);
            report["outcome"] = "compared";
            report["actual"] = actual.bits;
            report["expected"] = expected;
            report["valid"] = actual.valid;
            report["expected_valid"] = expectedValid;
            report["gpu_operations"] = inputs.gpu->operations();
            report["oracle_stage"] = 1;
            saveArithmeticEvidence(QString("raw-") + QTest::currentDataTag(), report, executable, shader);
            qInfo().noquote() << QString::fromStdString(nlohmann::json({{"opcode", opcode},
                                                                        {"stage", stage},
                                                                        {"form", form},
                                                                        {"expected", expected},
                                                                        {"actual", actual.bits},
                                                                        {"valid", actual.valid},
                                                                        {"mask", mask},
                                                                        {"identity", inputs.gpu->identity()}})
                                                            .dump());
            QCOMPARE(actual.bits, expected);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void doubleGuards() {
        const auto flags = op(106 | (1u << 12));
        const auto source = src(0, 1);
        for (uint32_t mask : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 14u})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     Program(binary({flags, op(199, {dst(0, 0, mask), source}), op(62)})));
        for (uint32_t mask : {0u, 7u, 11u, 13u, 14u, 15u})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     Program(binary({flags, op(201, {dst(0, 0, mask), source}), op(62)})));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Program(binary({op(199, {dst(0, 0), source}), op(62)})));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 Program(binary({flags, op(212, {dst(0, 0), source}), op(62)})));
        for (uint32_t selection : {0u, 1u, 27u, 255u}) {
            auto bad = source;
            bad[0] = (bad[0] & ~0xff0u) | (selection << 4);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                     Program(binary({flags, op(199, {dst(0, 0), bad}), op(62)})));
        }
        const Program program(binary({flags, op(199, {dst(0, 0), source}), op(62)}));
        Lane missing(program, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, missing.step());
        QVERIFY(missing.registers().empty());
    }
    void doublePairState_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(stage))) << vendor << stage;
    }
    void doublePairState() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        try {
            Device device(vendor);
            auto assist = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            D3D11_FEATURE_DATA_DOUBLES support{};
            check(device.device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &support, sizeof(support)),
                  "Read pair capability");
            const auto flags = op(106 | (1u << 12));
            const std::array<uint32_t, 4> original{0x12345678, 0xfff81234, 0x87654321, 0x00000000};
            for (uint32_t kind : {0u, 2u, 3u}) {
                if (kind == 2 && stage == 5)
                    continue;
                for (uint32_t selection : {0xe4u, 0x44u, 0x4eu, 0xeeu}) {
                    const Register target{kind, kind == 3 ? Words{0, 1} : Words{0}};
                    auto destination = dst(kind, 0);
                    if (kind == 3) {
                        destination[0] += 1u << 20;
                        destination.push_back(1);
                    }
                    for (uint32_t modifier : {0u, 1u, 2u, 3u}) {
                        Words source{0x208006 | (selection << 4), 0, 0};
                        if (modifier) {
                            source[0] |= 0x80000000u;
                            source.insert(source.begin() + 1, 1u | (modifier << 6));
                        }
                        std::vector<Words> rows{flags, op(89, {{0x208e46, 0, 1}}), op(104, {{1}}),
                                                op(105, {{0, 2, 4}})};
                        if (stage == 5)
                            rows.push_back(op(155, {{1, 1, 1}}));
                        if (kind == 2)
                            rows.push_back(op(101, {destination}));
                        rows.push_back(op(199, {destination, source}));
                        rows.push_back(op(62));
                        const auto bytes = writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows});
                        const Program program(makeDxbc({{0x58454853, bytes}}));
                        Inputs inputs;
                        inputs.gpu = assist;
                        inputs.constantBuffers[0] = {{original, 11}};
                        Lane lane(program, inputs);
                        if (!support.DoublePrecisionFloatShaderOps) {
                            QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
                            QVERIFY(lane.registers().empty());
                            continue;
                        }
                        const auto step = lane.step();
                        QVERIFY(step.instructionGpuAssisted);
                        Value expected;
                        for (unsigned pair = 0; pair < 2; ++pair) {
                            const auto first = (selection >> (4 * pair)) & 3;
                            expected.bits[2 * pair] = original[first];
                            expected.bits[2 * pair + 1] = original[first + 1];
                            if (modifier & 2)
                                expected.bits[2 * pair + 1] &= 0x7fffffff;
                            if (modifier & 1)
                                expected.bits[2 * pair + 1] ^= 0x80000000;
                            if (first == 0)
                                expected.valid |= uint8_t(3u << (2 * pair));
                        }
                        QCOMPARE(lane.value(target), expected);
                        QCOMPARE(step.changes.size(), size_t(1));
                        QCOMPARE(step.changes[0].before, Value{});
                        QCOMPARE(step.changes[0].after, expected);
                    }
                }
            }
            for (uint8_t conditionValid : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3)}) {
                const auto bytes = writeDxbcProgram(
                    {{(stage << 16) | 0x50, 0},
                     {flags, op(89, {{0x208e46, 0, 3}}), op(104, {{1}}),
                      op(200, {dst(0, 0), {0x208e46, 0, 0}, {0x208e46, 0, 1}, {0x208e46, 0, 2}}), op(62)}});
                const Program program(makeDxbc({{0x58454853, bytes}}));
                Inputs inputs;
                inputs.gpu = assist;
                inputs.constantBuffers[0] = {{{1, 0, 0, 1}, conditionValid}, {original, 3}, {original, 12}};
                Lane lane(program, inputs);
                if (!support.DoublePrecisionFloatShaderOps) {
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
                    continue;
                }
                lane.step();
                const auto actual = lane.value({0, {0}});
                QCOMPARE(actual.bits, original);
                QCOMPARE(actual.valid, uint8_t((conditionValid & 1 ? 3 : 0) | (conditionValid & 2 ? 12 : 0)));
            }
            // Invalid indexable component declarations fail before issuing any GPU work.
            const auto invalid = writeDxbcProgram(
                {{(stage << 16) | 0x50, 0},
                 {flags, op(105, {{0, 2, 2}}), op(199, {{0x2030f2, 0, 1}, {0x5002, 0, 0, 0, 0}}), op(62)}});
            Program bad(makeDxbc({{0x58454853, invalid}}));
            Inputs inputs;
            inputs.gpu = assist;
            Lane lane(bad, inputs);
            const auto count = assist->operations();
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
            QCOMPARE(assist->operations(), count);
            QVERIFY(lane.registers().empty());
            qInfo().noquote() << QString::fromStdString(
                nlohmann::json({{"pair_state_operations", count},
                                {"double_support", bool(support.DoublePrecisionFloatShaderOps)},
                                {"stage", stage},
                                {"identity", assist->identity()}})
                    .dump());
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void compiledDoubles_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("scenario");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto *scenario :
                 {"arithmetic", "minmax", "comparison", "tofloat", "fromfloat", "toint", "touint", "fromint",
                  "fromuint", "division", "conditional", "loop"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(vendor).arg(scenario)))
                    << vendor << QString(scenario);
    }
    void compiledDoubles() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, scenario);
        try {
            const std::map<QString, std::string> bodies{
                {"arithmetic", "d = a * b + c;"},
                {"minmax", "d = double2(min(a.x,b.x),max(a.y,b.y));"},
                {"comparison", "return uint4(a.x<b.x,a.y>=b.y,a.x==b.x,a.y!=b.y);"},
                {"tofloat", "return uint4(asuint(float2(a)), 0, 0);"},
                {"fromfloat", "d = double2(asfloat(x.xy));"},
                {"toint", "return uint4(int2(a),0,0);"},
                {"touint", "return uint4(uint2(a),0,0);"},
                {"fromint", "d = double2(asint(x.xy));"},
                {"fromuint", "d = double2(x.xy);"},
                {"division", "d = a / b;"},
                {"conditional", "d = a.x > b.x ? a : b;"},
                {"loop", "d=a;[loop]for(uint i=0;i<(z.x&7);++i){if(i==2)continue;d=d*b+c;}"}};
            const auto shader =
                compileHlsl("cbuffer Data:register(b0){uint4 x,y,z;}uint4 main():TEXCOORD0{"
                            "double2 a=double2(asdouble(x.x,x.y),asdouble(x.z,x.w));"
                            "double2 b=double2(asdouble(y.x,y.y),asdouble(y.z,y.w));"
                            "double2 c=double2(asdouble(z.x,z.y),asdouble(z.z,z.w));double2 d;" +
                                bodies.at(scenario) +
                                "uint2 lo,hi;asuint(d,lo,hi);return uint4(lo.x,hi.x,lo.y,hi.y);}",
                            "vs_5_0")
                    .bytecode;
            const Program program(shader);
            QVERIFY(std::any_of(
                program.instructions().begin(), program.instructions().end(), [](const Instruction &i) {
                    return (i.opcode >= 191 && i.opcode <= 202) || (i.opcode >= 210 && i.opcode <= 217);
                }));
            Device device(vendor);
            D3D11_FEATURE_DATA_DOUBLES support{};
            check(device.device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &support, sizeof(support)),
                  "Read compiled double capability");
            for (uint32_t dataset : {0u, 1u, 2u}) {
                std::vector<std::array<uint32_t, 4>> values{{0, 0x3ff80000, 0, 0xc0020000},
                                                            {0, 0x3fe00000, 0, 0x40100000},
                                                            {3, 0xbff00000, 0, 0x40000000}};
                if (dataset == 1)
                    values[0] = {1, 0, 0, 0x80000000};
                else if (dataset == 2)
                    values[0] = {0x12345678, 0x7ff80000, 0, 0x7ff00000};
                Inputs inputs;
                inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
                for (const auto &value : values)
                    inputs.constantBuffers[0].push_back(Value::known(value));
                Lane lane(program, inputs);
                nlohmann::json report{{"scope", "compiled_double_shader_semantics"},
                                      {"vendor", vendor},
                                      {"scenario", scenario.toStdString()},
                                      {"dataset", dataset},
                                      {"stage", 1},
                                      {"basic_support", bool(support.DoublePrecisionFloatShaderOps)},
                                      {"identity", inputs.gpu->identity()}};
                const auto evidenceName = QString("compiled-%1-%2").arg(QTest::currentDataTag()).arg(dataset);
                if (!support.DoublePrecisionFloatShaderOps) {
                    QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(lane));
                    report["outcome"] = "hardware_unsupported";
                    saveArithmeticEvidence(evidenceName, report, shader);
                    qInfo() << "Compiled double hardware unsupported: explicit rejection verified" << dataset;
                    continue;
                }
                const auto steps = execute(lane);
                auto restored = lane.registers();
                for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                    for (auto change = step->changes.rbegin(); change != step->changes.rend(); ++change) {
                        QCOMPARE(restored[change->destination], change->after);
                        restored[change->destination] = change->before;
                    }
                for (const auto &[reg, value] : restored)
                    QCOMPARE(value, Value{});
                std::vector<uint32_t> constants;
                for (const auto &value : values)
                    constants.insert(constants.end(), value.begin(), value.end());
                const auto expected = device.run(shader, constants);
                const auto actual = lane.value({2, {0}});
                report["outcome"] = "compared";
                report["actual"] = actual.bits;
                report["expected"] = expected;
                report["valid"] = actual.valid;
                report["gpu_operations"] = inputs.gpu->operations();
                report["reverse_journal_verified"] = true;
                saveArithmeticEvidence(evidenceName, report, shader, shader);
                qInfo().noquote() << QString::fromStdString(
                    nlohmann::json({{"scenario", scenario.toStdString()},
                                    {"dataset", dataset},
                                    {"expected", expected},
                                    {"actual", actual.bits},
                                    {"valid", actual.valid},
                                    {"gpu_operations", inputs.gpu->operations()},
                                    {"identity", inputs.gpu->identity()}})
                        .dump());
                QCOMPARE(actual.valid, uint8_t(15));
                QCOMPARE(actual.bits, expected);
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
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
    void minimumPrecision_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<QString>("scenario");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (const auto name :
                 {"float-cast", "signed-cast", "unsigned-cast", "float-mad", "float-dot", "native-mad",
                  "native-dot", "modifiers", "float-output", "signed-output", "unsigned-output"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(name).arg(vendor))) << vendor << QString(name);
    }
    void minimumPrecision() {
        QFETCH(uint32_t, vendor);
        QFETCH(QString, scenario);
        try {
            const std::map<QString, std::string> bodies{
                {"float-cast",
                 "min16float4 x=min16float4(asfloat(a))+min16float4(asfloat(b));return asuint(float4(x));"},
                {"signed-cast",
                 "min16int4 x=min16int4(asint(a))+min16int4(asint(b));return asuint(int4(x));"},
                {"unsigned-cast", "min16uint4 x=min16uint4(a)+min16uint4(b);return uint4(x);"},
                {"float-mad", "min16float4 "
                              "x=mad(min16float4(asfloat(a)),min16float4(asfloat(b)),min16float4(asfloat(c)))"
                              ";return asuint(float4(x));"},
                {"float-dot", "min16float x=dot(min16float4(asfloat(a)),min16float4(asfloat(b)));return "
                              "asuint(float4(x,x,x,x));"},
                {"native-mad", "precise float4 x=mad(asfloat(a),asfloat(b),asfloat(c));return asuint(x);"},
                {"native-dot", "precise float x=dot(asfloat(a),asfloat(b));return asuint(x.xxxx);"},
                {"modifiers", "min16float4 x=min16float4(asfloat(a));min16float4 "
                              "y=saturate(-abs(x)+min16float4(asfloat(b)));return asuint(float4(y));"},
                {"float-output", "return min16float4(asfloat(a))*min16float4(asfloat(b));"},
                {"signed-output", "return min16int4(asint(a))+min16int4(asint(b));"},
                {"unsigned-output", "return min16uint4(a)+min16uint4(b);"}};
            const std::string output = scenario == "float-output"      ? "min16float4"
                                       : scenario == "signed-output"   ? "min16int4"
                                       : scenario == "unsigned-output" ? "min16uint4"
                                                                       : "uint4";
            const auto code = compileHlsl("cbuffer Data:register(b0){uint4 a,b,c;}" + output +
                                              " main():TEXCOORD0{" + bodies.at(scenario) + "}",
                                          "vs_5_0")
                                  .bytecode;
            const std::array<std::array<uint32_t, 4>, 3> values{
                {{0x3eaaaaab, 0xbfaaaaab, 0x3f801001, 0x80000000},
                 {0x3faaaaab, 0x3f801001, 0x40000000, 0x3eaaaaab},
                 {0xbeaaaaab, 0x3f800001, 0xbf800001, 0x3eaaaaab}}};
            Device device(vendor);
            Inputs inputs;
            for (const auto &v : values)
                inputs.constantBuffers[0].push_back(Value::known(v));
            inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            Program program(code);
            Lane lane(program, inputs);
            const auto steps = execute(lane);
            QVERIFY(std::any_of(steps.begin(), steps.end(),
                                [](const Step &s) { return s.instructionGpuAssisted; }));
            const auto expected = device.run(code, {values.front().data(), 12});
            const auto actual = lane.value({2, {0}});
            qInfo().noquote() << QString::fromStdString(nlohmann::json({{"scenario", scenario.toStdString()},
                                                                        {"expected", expected},
                                                                        {"actual", actual.bits},
                                                                        {"identity", inputs.gpu->identity()}})
                                                            .dump());
            QCOMPARE(actual.valid, uint8_t(15));
            QCOMPARE(actual.bits, expected);
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
    void precisionGuards() {
        auto precise = [](Words arg, uint32_t precision) {
            arg[0] |= 0x80000000u;
            arg.insert(arg.begin() + 1, 1u | (precision << 14));
            return arg;
        };
        for (uint32_t precision : {3u, 6u, 7u})
            QVERIFY_THROWS_EXCEPTION(
                std::runtime_error,
                Program(binary({op(54, {precise(dst(2, 0), precision), literal(0)}), op(62)})));
        const Program program(binary({op(54, {precise(dst(2, 0), 1), literal(0x3f800000)}), op(62)}));
        Lane missing(program, {});
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, missing.step());
        QVERIFY(missing.registers().empty());
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error, Program(binary({op(31 | 0x40000, {precise(src(0, 0), 4)}), op(21), op(62)})));
        Device device(0);
        Inputs inputs;
        inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), true);
        for (uint32_t stage : {0u, 1u}) {
            const auto operation =
                stage == 0 ? op(54, {precise(dst(2, 0), 1), literal(0x3f800000)})
                           : op(77, {precise(dst(2, 0), 1), precise(dst(0, 0), 1), literal(0x3f800000)});
            const auto bytes = writeDxbcProgram(
                {{(stage << 16) | 0x50, 0}, {op(104, {{1}}), op(101, {dst(2, 0)}), operation, op(62)}});
            const Program unsupported(makeDxbc({{0x58454853, bytes}}));
            Lane lane(unsupported, inputs);
            const auto count = inputs.gpu->operations();
            if (stage == 0) {
                QVERIFY(lane.step().instructionGpuAssisted);
                QCOMPARE(lane.value({2, {0}}).bits,
                         (std::array<uint32_t, 4>{0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000}));
                QCOMPARE(lane.value({2, {0}}).valid, uint8_t(15));
            } else {
                QVERIFY_THROWS_EXCEPTION(std::runtime_error, lane.step());
                QVERIFY(lane.registers().empty());
                QCOMPARE(inputs.gpu->operations(), count);
            }
        }
    }
    void precisionStages_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("precision");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                for (uint32_t precision : {1u, 2u, 4u, 5u})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(vendor).arg(stage).arg(precision)))
                        << vendor << stage << precision;
    }
    void precisionStages() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, precision);
        try {
            auto annotate = [&](Words value) {
                value[0] |= 0x80000000u;
                value.insert(value.begin() + 1, 1u | (precision << 14));
                return value;
            };
            std::vector<Words> rows{op(106 | (1u << 16)), op(89, {{0x208e46, 0, 2}}), op(104, {{1}})};
            if (stage == 5)
                rows.push_back(op(155, {{1, 1, 1}}));
            rows.push_back(op(precision < 4 ? 0u : 30u, {annotate(dst(0, 0, 5)), annotate({0x208e46, 0, 0}),
                                                         annotate({0x208e46, 0, 1})}));
            rows.push_back(op(62));
            const auto executable = writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows});
            const auto shader = makeDxbc({{0x58454853, executable}});
            Device device(vendor);
            Inputs inputs;
            inputs.gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            const auto bits = precision < 4 ? 0x3fc00000u : 12u;
            inputs.constantBuffers[0] = {{{bits, 0, bits, 0}, 5}, {{bits, 0, bits, 0}, 5}};
            Program program(shader);
            Lane lane(program, inputs);
            const auto step = lane.step();
            QVERIFY(step.instructionGpuAssisted);
            const auto actual = lane.value({0, {0}});
            QCOMPARE(actual.valid, uint8_t(5));
            QCOMPARE(actual.bits[0], precision < 4 ? 0x40400000u : 24u);
            QCOMPARE(actual.bits[2], actual.bits[0]);
            QVERIFY(inputs.gpu->operations() > 0);
            if (precision >= 4) {
                // Precision conversion can turn 65536 into zero on some adapters.
                // Either possible selection must remain unknown when one branch is unknown.
                const auto conditional = writeDxbcProgram(
                    {{(stage << 16) | 0x50, 0},
                     {op(106 | (1u << 16)), op(89, {{0x208e46, 0, 3}}), op(104, {{1}}),
                      op(55, {dst(0, 0), annotate({0x208e46, 0, 0}), {0x208e46, 0, 1}, {0x208e46, 0, 2}}),
                      op(62)}});
                Program branch(makeDxbc({{0x58454853, conditional}}));
                inputs.constantBuffers[0] = {Value::known({65536, 65536, 65536, 65536}),
                                             Value::known({7, 7, 7, 7}), Value{}};
                Lane selection(branch, inputs);
                QVERIFY(selection.step().instructionGpuAssisted);
                QCOMPARE(selection.value({0, {0}}).valid, uint8_t(0));
            }
        } catch (const std::exception &e) {
            QFAIL(e.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcExecutionTests)
#include "DxbcExecutionTests.moc"
