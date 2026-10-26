#include "application/DxbcGpuCompletion.h"
#include "application/DxbcPixelGeometry.h"
#include "core/Dxbc.h"
#include "core/StreamOutput.h"
#include "replay/Replay.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <bit>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <iostream>
#include <set>
using namespace flora;
using namespace flora::dxbc_exec;
using Json = nlohmann::json;
namespace {
uint64_t number(const QString &value) {
    bool ok = false;
    const auto result = value.toULongLong(&ok, 0);
    if (!ok)
        throw std::runtime_error("Invalid pixel geometry probe number");
    return result;
}
void save(const QDir &directory, const QString &name, Bytes bytes) {
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) !=
            qint64(bytes.size()))
        throw std::runtime_error("Cannot save pixel geometry probe evidence");
}
void save(const QDir &directory, const QString &name, const Json &value) {
    const auto text = value.dump(2);
    save(directory, name, Bytes(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}
void shader(const QDir &directory, const QString &name, Bytes bytes) {
    save(directory, name + ".dxbc", bytes);
    Com<ID3DBlob> assembly;
    check(D3DDisassemble(bytes.data(), bytes.size(), 0, nullptr, &assembly),
          "Disassemble geometry probe shader");
    save(directory, name + ".asm.txt",
         Bytes(static_cast<const uint8_t *>(assembly->GetBufferPointer()), assembly->GetBufferSize()));
}
void validate(ID3D11Device *device) {
    Com<ID3D11InfoQueue> queue;
    check(device->QueryInterface(IID_PPV_ARGS(&queue)), "Query geometry validation");
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        check(queue->GetMessage(i, nullptr, &size), "Size geometry validation");
        std::vector<uint8_t> bytes(size);
        auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
        check(queue->GetMessage(i, message, &size), "Read geometry validation");
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    queue->ClearStoredMessages();
}
std::vector<uint8_t> roundtrip(ID3D11Device *device, const PixelGeometryBridge &bridge) {
    Com<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->ClearState();
    Com<ID3D11VertexShader> vs;
    check(device->CreateVertexShader(bridge.vertexShader.data(), bridge.vertexShader.size(), nullptr, &vs),
          "Create bridge probe VS");
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    for (UINT slot = 0; slot < bridge.stride / 16; ++slot)
        elements.push_back({"FLORA_GEOMETRY", slot, DXGI_FORMAT_R32G32B32A32_UINT, 0, slot * 16,
                            D3D11_INPUT_PER_VERTEX_DATA, 0});
    Com<ID3D11InputLayout> layout;
    check(device->CreateInputLayout(elements.data(), UINT(elements.size()), bridge.vertexShader.data(),
                                    bridge.vertexShader.size(), &layout),
          "Create bridge probe layout");
    std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
    UINT outputStride = 0;
    for (const auto &f : bridge.identity.at("output_mapping")) {
        const auto mask = f.at("mask").get<uint32_t>();
        for (uint32_t c = 0; c < 4; ++c)
            if (mask & (1u << c)) {
                entries.push_back({0, f.at("semantic").get_ref<const std::string &>().c_str(),
                                   f.at("index").get<UINT>(), BYTE(c - std::countr_zero(mask)), 1, 0});
                outputStride += 4;
            }
    }
    Com<ID3D11GeometryShader> gs;
    check(device->CreateGeometryShaderWithStreamOutput(
              bridge.geometryShader.data(), bridge.geometryShader.size(), entries.data(),
              UINT(entries.size()), &outputStride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs),
          "Create bridge probe SO shader");
    Com<ID3D11Buffer> vertices, output, staging;
    D3D11_BUFFER_DESC bd{
        UINT(bridge.vertices.size()), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA data{bridge.vertices.data(), 0, 0};
    check(device->CreateBuffer(&bd, &data, &vertices), "Create bridge probe vertices");
    bd.ByteWidth = outputStride * bridge.verticesPerPrimitive;
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    check(device->CreateBuffer(&bd, nullptr, &output), "Create bridge probe SO storage");
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateBuffer(&bd, nullptr, &staging), "Create bridge probe staging");
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->IASetInputLayout(layout.Get());
    const UINT offset = 0;
    context->IASetVertexBuffers(0, 1, vertices.GetAddressOf(), &bridge.stride, &offset);
    context->IASetPrimitiveTopology(bridge.verticesPerPrimitive == 1 ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                    : bridge.verticesPerPrimitive == 2
                                        ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                        : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->SOSetTargets(1, output.GetAddressOf(), &offset);
    context->Draw(bridge.verticesPerPrimitive, 0);
    context->SOSetTargets(0, nullptr, nullptr);
    context->CopyResource(staging.Get(), output.Get());
    GpuCompletion completion(device, context.Get());
    completion.submit();
    const auto mapped = completion.map(staging.Get());
    std::vector<uint8_t> bytes(bd.ByteWidth);
    std::memcpy(bytes.data(), mapped.pData, bytes.size());
    context->Unmap(staging.Get(), 0);
    context->ClearState();
    validate(device);
    size_t offsetOut = 0;
    for (uint32_t vertex = 0; vertex < bridge.verticesPerPrimitive; ++vertex)
        for (const auto &f : bridge.identity.at("output_mapping"))
            for (const auto &c : f.at("components")) {
                const auto pos = vertex * bridge.stride + f.at("slot").get<uint32_t>() * 16 +
                                 c.at("output_component").get<uint32_t>() * 4;
                if (Reader(Bytes(bytes).subspan(offsetOut)).read<uint32_t>() !=
                    Reader(Bytes(bridge.vertices).subspan(pos)).read<uint32_t>())
                    throw std::runtime_error("GPU bridge roundtrip changed raw component bits");
                offsetOut += 4;
            }
    return bytes;
}
} // namespace
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 6) {
        std::cerr << "Usage: FloraDxbcPixelGeometryProbe <frame> <event> <primitive-ordinal> "
                     "<vendor-or-0-for-WARP> <new-directory>\n";
        return 2;
    }
    try {
        const auto event = number(args[2]), ordinal = number(args[3]), vendor = number(args[4]);
        if (vendor > UINT32_MAX)
            throw std::runtime_error("Adapter vendor is outside uint32");
        QDir directory(args[5]);
        if (directory.exists() || !QDir().mkpath(args[5]))
            throw std::runtime_error("Probe directory must be new");
        QElapsedTimer timer;
        timer.start();
        Frame frame(args[1].toStdWString());
        ReplayOptions options;
        options.until = event;
        options.before = options.prepareBeforeDraw = true;
        options.warp = !vendor;
        if (vendor)
            options.vendor = uint32_t(vendor);
        Replay replay(frame, options);
        replay.run([&](Id, size_t, size_t) {
            if (timer.elapsed() > 120000)
                throw std::runtime_error("Geometry probe replay deadline");
        });
        const auto state = frame.state(frame.event(event).state);
        const auto ps = frame.shader(frame.resource(state.stages[4].shader).data);
        PostTransformOptions geometryOptions;
        if (state.stages[3].shader)
            if (const auto declaration = shaderStreamOutput(frame, state.stages[3].shader))
                geometryOptions.stream = readStreamOutputDeclaration(frame, declaration).rasterizedStream;
        if (geometryOptions.stream == UINT32_MAX)
            throw std::runtime_error("Selected draw has no rasterized GS stream");
        const auto geometry = inspectPostTransform(replay, event, geometryOptions);
        save(directory, "source-geometry.json", geometry.report);
        save(directory, "source-geometry.bin", Bytes(geometry.bytes));
        shader(directory, "original-ps", ps);
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> adapter;
        check(replay.nativeDevice()->QueryInterface(IID_PPV_ARGS(&dxgi)), "Query reference adapter");
        check(dxgi->GetAdapter(&adapter), "Get reference adapter");
        Com<ID3D11Device> device;
        check(D3D11CreateDevice(vendor ? adapter.Get() : nullptr,
                                vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP, nullptr,
                                D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                                nullptr),
              "Create private bridge probe device");
        Com<IDXGIDevice> privateDxgi;
        Com<IDXGIAdapter> privateAdapter;
        DXGI_ADAPTER_DESC expected{}, actual{};
        check(adapter->GetDesc(&expected), "Describe reference adapter");
        check(device.As(&privateDxgi), "Query private adapter");
        check(privateDxgi->GetAdapter(&privateAdapter), "Get private adapter");
        check(privateAdapter->GetDesc(&actual), "Describe private adapter");
        if (std::memcmp(&actual.AdapterLuid, &expected.AdapterLuid, sizeof(LUID)))
            throw std::runtime_error("Private bridge adapter differs");
        std::set<uint32_t> registers;
        for (const auto &[tag, bytes] : readDxbcParts(ps))
            if (tag == 0x52444853 || tag == 0x58454853)
                for (const auto &row : readDxbcProgram(bytes).instructions)
                    if ((row[0] & 2047) >= 98 && (row[0] & 2047) <= 100 && ((row.at(1) >> 12) & 255) == 1)
                        registers.insert(row.at((row[1] & 0x80000000u) ? 3 : 2));
        Json report{{"schema", 1},
                    {"frame_sha256", frame.sha256()},
                    {"event", event},
                    {"primitive_ordinal", ordinal},
                    {"adapter_vendor", actual.VendorId},
                    {"adapter_device", actual.DeviceId},
                    {"adapter_luid",
                     (uint64_t(uint32_t(actual.AdapterLuid.HighPart)) << 32) | actual.AdapterLuid.LowPart},
                    {"cases", Json::array()},
                    {"original_fragment_eligibility", false}};
        bool complete = true;
        for (auto reg : registers) {
            const auto name = "input-" + QString::number(reg);
            directory.mkdir(name);
            QDir target(directory.filePath(name));
            Json item{{"register", reg}, {"status", "ok"}};
            try {
                const auto input = pixelInputShader(ps, reg);
                const auto bridge = pixelGeometryBridge(geometry, input, ordinal);
                save(target, "bridge.json", bridge.identity);
                save(target, "vertices.bin", Bytes(bridge.vertices));
                shader(target, "vertex", bridge.vertexShader);
                shader(target, "geometry", bridge.geometryShader);
                shader(target, "pixel", input.bytecode);
                const auto raw = roundtrip(device.Get(), bridge);
                save(target, "roundtrip.bin", Bytes(raw));
                item["roundtrip_components"] = raw.size() / 4;
            } catch (const std::exception &e) {
                item["status"] = "failed";
                item["error"] = e.what();
                complete = false;
            }
            report["cases"].push_back(item);
        }
        report["status"] = registers.empty() ? "inapplicable_no_declared_inputs" : complete ? "ok" : "failed";
        report["elapsed_ms"] = timer.elapsed();
        save(directory, "report.json", report);
        std::cout << report.dump(2) << '\n';
        return registers.empty() ? 3 : complete ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
