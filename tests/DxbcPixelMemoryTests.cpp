#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <chrono>
#include <d3d11sdklayers.h>
#include <set>
#include <thread>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
using Words = std::vector<uint32_t>;
using Pixels = std::array<std::array<uint32_t, 4>, 4>;
Words row(uint32_t opcode, std::initializer_list<Words> args = {}) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words reg(uint32_t kind, uint32_t index, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), index};
}
Words source(uint32_t kind, uint32_t index) { return {0x100e46 | (kind << 12), index}; }
Words imm(uint32_t value) { return {0x4001, value}; }
std::vector<uint8_t> bytes(const Words &words) {
    std::vector<uint8_t> result(words.size() * 4);
    std::memcpy(result.data(), words.data(), result.size());
    return result;
}
std::string shader(bool structured, uint32_t operation) {
    if (operation >= 4) {
        const std::string counter = operation == 4 ? "IncrementCounter()" : "DecrementCounter()";
        return "StructuredBuffer<uint> input:register(t0);RWStructuredBuffer<uint> data:register(u1);"
               "cbuffer Params:register(b0){uint mask;}uint4 main(float4 p:SV_Position):SV_Target{"
               "uint i=(uint(p.x)&1)+2*(uint(p.y)&1);float v=asfloat(input[i]);"
               "float2 gradients=float2(ddx_fine(v),ddy_fine(v));uint before=data." +
               counter + ";data[i]=101+i;if(mask&(1u<<i))discard;uint after=data." + counter +
               ";return uint4(asuint(gradients),before,after);}";
    }
    const auto input = structured ? "asfloat(input[i])" : "asfloat(input.Load(4*i))";
    const auto address = structured ? "data[4+i]" : "16+4*i";
    std::string body;
    if (operation == 0)
        body =
            structured ? "old=data[4+i];data[4+i]=old+3;" : "old=data.Load(16+4*i);data.Store(16+4*i,old+3);";
    else {
        const auto prefix = structured ? "Interlocked" : "data.Interlocked";
        body = std::string(prefix) +
               (operation == 1   ? "Add("
                : operation == 2 ? "Exchange("
                                 : "CompareExchange(") +
               address +
               (operation == 1   ? ",3,old);"
                : operation == 2 ? ",42,old);"
                                 : ",50+i,42,old);");
    }
    return std::string(
               structured
                   ? "StructuredBuffer<uint> input:register(t0);RWStructuredBuffer<uint> data:register(u1);"
                   : "ByteAddressBuffer input:register(t0);RWByteAddressBuffer data:register(u1);") +
           "cbuffer Params:register(b0){uint mask;}uint4 main(float4 p:SV_Position):SV_Target{"
           "uint i=(uint(p.x)&1)+2*(uint(p.y)&1);" +
           (structured ? "data[i]=101+i;" : "data.Store(4*i,101+i);") +
           "if(mask&(1u<<i))discard;float v=" + input + ";uint old;" + body +
           "uint current=" + (structured ? "data[4+i]" : "data.Load(16+4*i)") +
           ";return uint4(asuint(ddx_fine(v)),asuint(ddy_fine(v)),old,current);}";
}
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    DXGI_ADAPTER_DESC identity{};
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create pixel memory factory");
            for (UINT i = 0;; ++i) {
                Com<IDXGIAdapter1> adapter;
                auto hr = factory->EnumAdapters1(i, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate pixel memory adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Read pixel memory adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested pixel memory adapter is absent");
        }
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create pixel memory reference device");
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> adapter;
        check(device.As(&dxgi), "Query pixel memory DXGI device");
        check(dxgi->GetAdapter(&adapter), "Read pixel memory device adapter");
        check(adapter->GetDesc(&identity), "Read pixel memory device identity");
    }
    struct Observation {
        Pixels pixels;
        Words words;
        std::optional<uint32_t> count;
    };
    Observation run(Bytes code, bool structured, uint32_t coverage, uint32_t discarded, bool counter) {
        context->ClearState();
        auto vertexCode =
            compileHlsl(
                "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==1?3:-1,i==2?3:-1,0,1);}",
                "vs_5_0")
                .bytecode;
        Com<ID3D11VertexShader> vertex;
        Com<ID3D11PixelShader> pixel;
        check(device->CreateVertexShader(vertexCode.data(), vertexCode.size(), nullptr, &vertex),
              "Create memory VS");
        check(device->CreatePixelShader(code.data(), code.size(), nullptr, &pixel), "Create memory PS");
        const Words initialInput{0x3f800000, 0x40400000, 0x41000000, 0x41a00000};
        const Words initialData{7, 8, 9, 10, 50, 51, 52, 53};
        const auto buffer = [&](const Words &values, UINT bind) {
            D3D11_BUFFER_DESC desc{UINT(values.size() * 4),
                                   D3D11_USAGE_DEFAULT,
                                   bind,
                                   0,
                                   structured ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                              : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
                                   structured ? 4u : 0u};
            D3D11_SUBRESOURCE_DATA initial{values.data(), 0, 0};
            Com<ID3D11Buffer> result;
            check(device->CreateBuffer(&desc, &initial, &result), "Create memory fixture buffer");
            return result;
        };
        auto input = buffer(initialInput, D3D11_BIND_SHADER_RESOURCE);
        auto data = buffer(initialData, D3D11_BIND_UNORDERED_ACCESS);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        srv.BufferEx.NumElements = 4;
        srv.BufferEx.Flags = structured ? 0u : D3D11_BUFFEREX_SRV_FLAG_RAW;
        Com<ID3D11ShaderResourceView> inputView;
        check(device->CreateShaderResourceView(input.Get(), &srv, &inputView), "Create memory input view");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 8;
        uav.Buffer.Flags = counter      ? D3D11_BUFFER_UAV_FLAG_COUNTER
                           : structured ? 0u
                                        : D3D11_BUFFER_UAV_FLAG_RAW;
        Com<ID3D11UnorderedAccessView> dataView;
        check(device->CreateUnorderedAccessView(data.Get(), &uav, &dataView), "Create memory output view");
        D3D11_TEXTURE2D_DESC td{
            2, 2, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET,
            0, 0};
        Com<ID3D11Texture2D> target, pixelsRead;
        Com<ID3D11RenderTargetView> targetView;
        check(device->CreateTexture2D(&td, nullptr, &target), "Create memory target");
        check(device->CreateRenderTargetView(target.Get(), nullptr, &targetView),
              "Create memory target view");
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&td, nullptr, &pixelsRead), "Create memory target staging");
        D3D11_BUFFER_DESC bd{32, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        Com<ID3D11Buffer> dataRead, constants, countBuffer, countRead;
        check(device->CreateBuffer(&bd, nullptr, &dataRead), "Create memory data staging");
        if (counter) {
            bd.ByteWidth = 4;
            check(device->CreateBuffer(&bd, nullptr, &countRead), "Create counter staging");
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.CPUAccessFlags = 0;
            check(device->CreateBuffer(&bd, nullptr, &countBuffer), "Create counter destination");
        }
        bd = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        const std::array<uint32_t, 4> params{discarded, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA cb{params.data(), 0, 0};
        check(device->CreateBuffer(&bd, &cb, &constants), "Create memory constants");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = true;
        rd.ScissorEnable = true;
        Com<ID3D11RasterizerState> raster;
        check(device->CreateRasterizerState(&rd, &raster), "Create memory raster state");
        D3D11_VIEWPORT viewport{0, 0, 2, 2, 0, 1};
        D3D11_RECT rect{0, 0, coverage == 5 || coverage == 1 ? 1 : 2, coverage == 3 || coverage == 1 ? 1 : 2};
        context->RSSetState(raster.Get());
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &rect);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, inputView.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
        const UINT initialCount = 8;
        context->OMSetRenderTargetsAndUnorderedAccessViews(1, targetView.GetAddressOf(), nullptr, 1, 1,
                                                           dataView.GetAddressOf(),
                                                           counter ? &initialCount : nullptr);
        const float clear[4]{};
        context->ClearRenderTargetView(targetView.Get(), clear);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);
        context->CopyResource(dataRead.Get(), data.Get());
        context->CopyResource(pixelsRead.Get(), target.Get());
        if (counter) {
            context->CopyStructureCount(countBuffer.Get(), 0, dataView.Get());
            context->CopyResource(countRead.Get(), countBuffer.Get());
        }
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
        Com<ID3D11Query> query;
        check(device->CreateQuery(&queryDesc, &query), "Create memory completion query");
        context->End(query.Get());
        context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (true) {
            auto hr = context->GetData(query.Get(), nullptr, 0, 0);
            check(hr, "Wait for memory reference");
            if (hr == S_OK)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Pixel memory reference timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Pixels pixels;
        Words words(8);
        check(context->Map(pixelsRead.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read memory pixels");
        for (unsigned y = 0; y < 2; ++y)
            std::memcpy(pixels.data() + 2 * y, static_cast<uint8_t *>(mapped.pData) + y * mapped.RowPitch,
                        32);
        context->Unmap(pixelsRead.Get(), 0);
        check(context->Map(dataRead.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read memory writes");
        std::memcpy(words.data(), mapped.pData, 32);
        context->Unmap(dataRead.Get(), 0);
        std::optional<uint32_t> count;
        if (counter) {
            check(context->Map(countRead.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read hidden counter");
            count = *static_cast<uint32_t *>(mapped.pData);
            context->Unmap(countRead.Get(), 0);
        }
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read memory validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read memory validation size");
            std::vector<uint8_t> storage(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(queue->GetMessage(i, message, &size), "Read memory validation");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
        queue->ClearStoredMessages();
        return {pixels, words, count};
    }
};
} // namespace
class DxbcPixelMemoryTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
    std::map<uint32_t, std::unique_ptr<Device>> devices;
    std::map<uint32_t, std::shared_ptr<GpuOperations>> gpu;
  private slots:
    void initTestCase() {
        for (uint32_t vendor : {0u, 4318u, 32902u}) {
            devices[vendor] = std::make_unique<Device>(vendor);
            gpu[vendor] = std::make_shared<GpuOperations>(devices.at(vendor)->device.Get(), vendor == 0);
        }
        if (qEnvironmentVariableIsSet("FLORA_KEEP_PIXEL_MEMORY_EVIDENCE")) {
            evidence.setAutoRemove(false);
            qInfo().noquote() << "Pixel memory evidence:" << evidence.path();
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<bool>("structured");
        QTest::addColumn<uint32_t>("coverage");
        QTest::addColumn<uint32_t>("discarded");
        QTest::addColumn<uint32_t>("operation");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (bool structured : {false, true})
                for (uint32_t coverage : {15u, 3u, 5u, 1u})
                    for (uint32_t discarded : {0u, 1u, 2u, 5u, 15u})
                        for (uint32_t operation : (structured ? Words{0, 1, 2, 3, 4, 5} : Words{0, 1, 2, 3}))
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(structured)
                                                         .arg(coverage)
                                                         .arg(discarded)
                                                         .arg(operation)))
                                << vendor << structured << coverage << discarded << operation;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(bool, structured);
        QFETCH(uint32_t, coverage);
        QFETCH(uint32_t, discarded);
        QFETCH(uint32_t, operation);
        try {
            auto code = compileHlsl(shader(structured, operation), "ps_5_0").bytecode;
            auto [expectedPixels, expectedWords, expectedCount] =
                devices.at(vendor)->run(code, structured, coverage, discarded, operation >= 4);
            const Program program(code);
            auto memory = std::make_shared<Memory>(
                std::map<uint64_t, Buffer>{
                    {1, Buffer::known(bytes({0x3f800000, 0x40400000, 0x41000000, 0x41a00000}))},
                    {2, Buffer::known(bytes({7, 8, 9, 10, 50, 51, 52, 53}))}},
                std::map<Register, BufferView>{
                    {{7, {0}}, {1, 0, 16, structured ? 4u : 0u}},
                    {{30, {1}},
                     {2, 0, 32, structured ? 4u : 0u,
                      operation >= 4 ? std::optional<uint64_t>(9) : std::nullopt}}},
                1024, 1024, nullptr, false,
                operation >= 4 ? std::map<uint64_t, Counter>{{9, {CounterKind::Ordered, 8}}}
                               : std::map<uint64_t, Counter>{});
            std::array<Inputs, 4> inputs;
            for (unsigned i = 0; i < 4; ++i) {
                inputs[i].memory = memory;
                inputs[i].gpu = gpu.at(vendor);
                inputs[i].constantBuffers[0] = {Value::known({discarded, 0, 0, 0})};
                inputs[i].registers[{1, {0}}] =
                    Value::known({std::bit_cast<uint32_t>(float(i % 2) + .5f),
                                  std::bit_cast<uint32_t>(float(i / 2) + .5f), 0, 0x3f800000});
            }
            PixelQuad quad(program, inputs, uint8_t(coverage));
            uint64_t helperMemory = 0, srvHelpers = 0;
            std::vector<PixelQuadStep> trace;
            while (!quad.finished()) {
                auto step = quad.step();
                const auto opcode = program.instructions().at(step.step.instruction).opcode;
                if (step.helper) {
                    QVERIFY(step.step.memoryChanges.empty());
                    QVERIFY(step.step.textureChanges.empty());
                    QVERIFY(step.step.counterChanges.empty());
                    if (opcode >= 163 && opcode <= 189)
                        ++helperMemory;
                    if (opcode == 165 || opcode == 167) {
                        const auto &words = program.instructions().at(step.step.instruction).words;
                        size_t operand = 1;
                        while (words.at(operand - 1) >> 31)
                            ++operand;
                        const auto written = (words.at(operand) >> 4) & 15;
                        for (const auto &change : step.step.changes)
                            if ((change.after.valid & written) == written)
                                ++srvHelpers;
                    }
                }
                trace.push_back(std::move(step));
            }
            QCOMPARE(quad.activeMask(), uint8_t(coverage & ~discarded));
            Pixels actualPixels{};
            for (unsigned i = 0; i < 4; ++i)
                if (quad.activeMask() & (1u << i)) {
                    auto v = quad.lane(i).value({2, {0}});
                    QCOMPARE(v.valid, uint8_t(15));
                    actualPixels[i] = v.bits;
                    if (operation < 4)
                        QCOMPARE(v.bits, expectedPixels[i]);
                    else {
                        QCOMPARE(v.bits[0], expectedPixels[i][0]);
                        QCOMPARE(v.bits[1], expectedPixels[i][1]);
                    }
                }
            QCOMPARE(memory->buffers().at(2).bytes, bytes(expectedWords));
            if (operation >= 4) {
                const auto count =
                    8 + (operation == 4 ? 1 : -1) *
                            (std::popcount(coverage) + std::popcount(uint32_t(quad.activeMask())));
                QCOMPARE(expectedCount, std::optional<uint32_t>(uint32_t(count)));
                QCOMPARE(memory->counters().at(9).value, expectedCount);
                for (const auto &pixels : {expectedPixels, actualPixels}) {
                    std::set<uint32_t> seen;
                    for (unsigned i = 0; i < 4; ++i)
                        if (quad.activeMask() & (1u << i)) {
                            QVERIFY(operation == 4 ? pixels[i][2] < pixels[i][3]
                                                   : pixels[i][2] > pixels[i][3]);
                            for (unsigned c : {2u, 3u}) {
                                QVERIFY(pixels[i][c] >= uint32_t(operation == 4 ? 8 : count));
                                QVERIFY(pixels[i][c] < uint32_t(operation == 4 ? count : 8));
                                QVERIFY(seen.insert(pixels[i][c]).second);
                            }
                        }
                }
                auto rewindCount = memory->counters().at(9);
                for (auto step = trace.rbegin(); step != trace.rend(); ++step)
                    for (auto change = step->step.counterChanges.rbegin();
                         change != step->step.counterChanges.rend(); ++change) {
                        QCOMPARE(change->view, uint64_t(9));
                        QCOMPARE(rewindCount, change->after);
                        rewindCount = change->before;
                    }
                QCOMPARE(rewindCount.value, std::optional<uint32_t>(8));
            }
            if (coverage != 15 || discarded) {
                QVERIFY(helperMemory > 0);
                if (operation < 4 || coverage != 15)
                    QVERIFY(srvHelpers > 0);
            }
            auto rewind = memory->buffers().at(2).bytes;
            for (auto step = trace.rbegin(); step != trace.rend(); ++step)
                for (auto change = step->step.memoryChanges.rbegin();
                     change != step->step.memoryChanges.rend(); ++change) {
                    QCOMPARE(change->resource, uint64_t(2));
                    std::copy(change->before.begin(), change->before.end(), rewind.begin() + change->offset);
                }
            QCOMPARE(rewind, bytes({7, 8, 9, 10, 50, 51, 52, 53}));
            nlohmann::json report{{"scope", "pixel_helper_memory"},
                                  {"vendor", vendor},
                                  {"structured", structured},
                                  {"coverage", coverage},
                                  {"discarded", discarded},
                                  {"operation", operation},
                                  {"model", PixelQuad::executionModel},
                                  {"output_scope", "active_pixels_only"},
                                  {"active_mask", quad.activeMask()},
                                  {"gpu", gpu.at(vendor)->identity()},
                                  {"expected_pixels", expectedPixels},
                                  {"actual_pixels", actualPixels},
                                  {"expected_words", expectedWords},
                                  {"actual_bytes", memory->buffers().at(2).bytes},
                                  {"helper_memory_steps", helperMemory},
                                  {"helper_srv_reads", srvHelpers}};
            report["expected_counter"] =
                expectedCount ? nlohmann::json(*expectedCount) : nlohmann::json(nullptr);
            report["actual_counter"] =
                operation >= 4 ? nlohmann::json(*memory->counters().at(9).value) : nlohmann::json(nullptr);
            const auto &identity = devices.at(vendor)->identity;
            report["reference_adapter"] = {{"vendor_id", identity.VendorId},
                                           {"device_id", identity.DeviceId},
                                           {"luid_low", identity.AdapterLuid.LowPart},
                                           {"luid_high", identity.AdapterLuid.HighPart}};
            const auto name = QString::fromLatin1(QTest::currentDataTag());
            QFile binary(evidence.filePath(name + ".dxbc"));
            QVERIFY(binary.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(binary.write(reinterpret_cast<const char *>(code.data()), qint64(code.size())),
                     qint64(code.size()));
            QFile file(evidence.filePath(name + ".json"));
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            const auto text = report.dump(2);
            QCOMPARE(file.write(text.data(), qint64(text.size())), qint64(text.size()));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void allSuppressedOpcodes_data() {
        QTest::addColumn<uint32_t>("opcode");
        QTest::addColumn<bool>("append");
        QTest::addColumn<bool>("typedAtomic");
        for (uint32_t op = 163; op <= 189; ++op)
            QTest::newRow(qPrintable(QString::number(op))) << op << false << false;
        for (uint32_t op : {178u, 179u})
            QTest::newRow(qPrintable(QString("%1-append").arg(op))) << op << true << false;
        for (uint32_t op = 169; op <= 189; ++op)
            if (op != 178 && op != 179)
                QTest::newRow(qPrintable(QString("%1-typed").arg(op))) << op << false << true;
    }
    void allSuppressedOpcodes() {
        QFETCH(uint32_t, opcode);
        QFETCH(bool, append);
        QFETCH(bool, typedAtomic);
        const bool typed = opcode == 163 || opcode == 164 || typedAtomic,
                   structured = opcode == 167 || opcode == 168 || opcode == 178 || opcode == 179;
        const bool returned = opcode == 163 || opcode == 165 || opcode == 167 || opcode >= 178;
        std::vector<Words> rows{
            row(104, {{3}}),
            typed ? row(156 | (1u << 11), {reg(30, 1), {0x4444}})
            : structured
                ? row(158 | ((opcode == 178 || opcode == 179) && !append ? 0x800000u : 0u), {reg(30, 1), {4}})
                : row(157, {reg(30, 1)}),
            row(54, {reg(0, 0), {0x4002, 11, 22, 33, 44}})};
        const auto unknown = source(0, 1), target = reg(0, 0, opcode >= 178 ? 4 : 5), resource = reg(30, 1);
        if (opcode == 163 || opcode == 165)
            rows.push_back(row(opcode, {target, unknown, source(30, 1)}));
        else if (opcode == 164 || opcode == 166)
            rows.push_back(row(opcode, {resource, unknown, unknown}));
        else if (opcode == 167)
            rows.push_back(row(opcode, {target, unknown, unknown, source(30, 1)}));
        else if (opcode == 168)
            rows.push_back(row(opcode, {resource, unknown, unknown, unknown}));
        else if (opcode == 178 || opcode == 179)
            rows.push_back(row(opcode, {target, source(30, 1)}));
        else if (opcode < 178)
            rows.push_back(opcode == 172 ? row(opcode, {resource, unknown, unknown, unknown})
                                         : row(opcode, {resource, unknown, unknown}));
        else
            rows.push_back(opcode == 185 ? row(opcode, {target, resource, unknown, unknown, unknown})
                                         : row(opcode, {target, resource, unknown, unknown}));
        rows.push_back(row(190 | 0x4000));
        rows.push_back(row(62));
        const Program program(makeDxbc({{0x58454853, writeDxbcProgram({{0x50, 0}, rows})}}));
        const auto initial = bytes({7, 8, 9, 10});
        auto memory = std::make_shared<Memory>(
            std::map<uint64_t, Buffer>{{1, Buffer::known(initial)}},
            std::map<Register, BufferView>{
                {{30, {1}},
                 {1, 0, 16, structured ? 4u : 0u,
                  (opcode == 178 || opcode == 179) ? std::optional<uint64_t>(9) : std::nullopt,
                  typed ? 42u : 0u}}},
            1024, 1024, nullptr, false,
            (opcode == 178 || opcode == 179)
                ? std::map<uint64_t, Counter>{{9, {append ? CounterKind::Append : CounterKind::Ordered, 3}}}
                : std::map<uint64_t, Counter>{});
        std::array<Inputs, 4> inputs;
        for (auto &input : inputs)
            input.memory = memory;
        PixelQuad quad(program, inputs, 0);
        while (!quad.finished()) {
            auto step = quad.step();
            QVERIFY(step.helper);
            QVERIFY(step.step.memoryChanges.empty());
            QVERIFY(step.step.textureChanges.empty());
            QVERIFY(step.step.counterChanges.empty());
        }
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto value = quad.lane(lane).value({0, {0}});
            QCOMPARE(value.valid, uint8_t(returned ? (opcode >= 178 ? 11 : 10) : 15));
            QCOMPARE(value.bits[1], 22u);
            QCOMPARE(value.bits[3], 44u);
        }
        QCOMPARE(memory->buffers().at(1).bytes, initial);
        if (opcode == 178 || opcode == 179)
            QCOMPARE(memory->counters().at(9).value, std::optional<uint32_t>(3));
    }
};
QTEST_GUILESS_MAIN(DxbcPixelMemoryTests)
#include "DxbcPixelMemoryTests.moc"
