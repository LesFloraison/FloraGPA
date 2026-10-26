#include "application/DxbcCheckpointModel.h"
#include "application/DxbcExecution.h"
#include "application/DxbcGpuOperations.h"
#include "application/DxbcTypedBuffer.h"
#include "application/HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <bit>
#include <d3d11sdklayers.h>
using namespace flora;
using namespace flora::dxbc_exec;
namespace {
struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11VertexShader> vertex;
    Com<ID3D11PixelShader> pixel;
    Com<ID3D11Buffer> buffer, constants;
    Com<ID3D11Texture2D> output;
    Com<ID3D11ShaderResourceView> view;
    Com<ID3D11RenderTargetView> target;
    Com<ID3D11RasterizerState> rasterizer;
    Com<ID3D11DepthStencilState> depth;
    Com<ID3D11BlendState> blend;
    DXGI_ADAPTER_DESC adapterIdentity{};
    explicit Device(uint32_t vendor) {
        Com<IDXGIAdapter1> selected;
        if (vendor) {
            Com<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create pixel probe factory");
            for (UINT index = 0;; ++index) {
                Com<IDXGIAdapter1> adapter;
                const auto hr = factory->EnumAdapters1(index, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "Enumerate pixel probe adapter");
                DXGI_ADAPTER_DESC1 desc{};
                check(adapter->GetDesc1(&desc), "Read pixel probe adapter");
                if (desc.VendorId == vendor && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    selected = adapter;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("Requested pixel probe adapter is absent");
        }
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        check(D3D11CreateDevice(selected.Get(), vendor ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
                                nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device,
                                nullptr, &context),
              "Create typed pixel reference device");
        Com<IDXGIDevice> dxgi;
        Com<IDXGIAdapter> actualAdapter;
        check(device.As(&dxgi), "Read reference DXGI device");
        check(dxgi->GetAdapter(&actualAdapter), "Read reference adapter");
        check(actualAdapter->GetDesc(&adapterIdentity), "Read reference adapter identity");
    }
    void validate() {
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read pixel probe validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read pixel validation size");
            std::vector<uint8_t> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            check(queue->GetMessage(i, message, &size), "Read pixel validation message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(message->pDescription);
        }
    }
    std::vector<uint8_t> read(ID3D11Buffer *resource) {
        D3D11_BUFFER_DESC desc{};
        resource->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;
        check(device->CreateBuffer(&desc, nullptr, &staging), "Create pixel reference buffer readback");
        context->CopyResource(staging.Get(), resource);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read pixel reference buffer");
        std::vector<uint8_t> result(desc.ByteWidth);
        std::memcpy(result.data(), mapped.pData, result.size());
        context->Unmap(staging.Get(), 0);
        return result;
    }
    void bind() {
        auto *cb = constants.Get();
        auto *srv = view.Get();
        auto *rtv = target.Get();
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        const D3D11_RECT scissor{0, 0, 1, 1};
        const FLOAT factor[]{0.2f, 0.3f, 0.4f, 0.5f};
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetShaderResources(3, 1, &srv);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &scissor);
        context->OMSetDepthStencilState(depth.Get(), 37);
        context->OMSetBlendState(blend.Get(), factor, UINT32_MAX);
        context->OMSetRenderTargets(1, &rtv, nullptr);
    }
    bool bindingsPreserved() {
        Com<ID3D11VertexShader> vs;
        Com<ID3D11PixelShader> ps;
        Com<ID3D11Buffer> cb;
        Com<ID3D11ShaderResourceView> srv;
        Com<ID3D11RenderTargetView> rtv;
        Com<ID3D11RasterizerState> rs;
        Com<ID3D11DepthStencilState> ds;
        Com<ID3D11BlendState> bs;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        D3D11_VIEWPORT viewport{};
        D3D11_RECT scissor{};
        UINT viewportCount = 1, scissorCount = 1, stencil = 0, mask = 0;
        std::array<float, 4> factor{};
        context->VSGetShader(&vs, nullptr, nullptr);
        context->PSGetShader(&ps, nullptr, nullptr);
        context->PSGetConstantBuffers(0, 1, &cb);
        context->PSGetShaderResources(3, 1, &srv);
        context->OMGetRenderTargets(1, &rtv, nullptr);
        context->OMGetDepthStencilState(&ds, &stencil);
        context->OMGetBlendState(&bs, factor.data(), &mask);
        context->RSGetState(&rs);
        context->RSGetViewports(&viewportCount, &viewport);
        context->RSGetScissorRects(&scissorCount, &scissor);
        context->IAGetPrimitiveTopology(&topology);
        return vs.Get() == vertex.Get() && ps.Get() == pixel.Get() && cb.Get() == constants.Get() &&
               srv.Get() == view.Get() && rtv.Get() == target.Get() && rs.Get() == rasterizer.Get() &&
               ds.Get() == depth.Get() && bs.Get() == blend.Get() && stencil == 37 && mask == UINT32_MAX &&
               factor == std::array<float, 4>{0.2f, 0.3f, 0.4f, 0.5f} && viewportCount == 1 &&
               scissorCount == 1 && viewport.TopLeftX == 0 && viewport.TopLeftY == 0 && viewport.Width == 1 &&
               viewport.Height == 1 && viewport.MinDepth == 0 && viewport.MaxDepth == 1 &&
               scissor.left == 0 && scissor.top == 0 && scissor.right == 1 && scissor.bottom == 1 &&
               topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    std::array<uint32_t, 4> readTarget() {
        D3D11_TEXTURE2D_DESC desc{};
        output->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Create pixel reference readback");
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read pixel reference target");
        std::array<uint32_t, 4> result;
        std::memcpy(result.data(), mapped.pData, 16);
        context->Unmap(staging.Get(), 0);
        return result;
    }
    std::array<uint32_t, 4> runOtherStage(Bytes code, Bytes storage, uint32_t format, uint32_t index,
                                          uint32_t stage, bool typedUav) {
        D3D11_BUFFER_DESC desc{UINT(storage.size()),
                               D3D11_USAGE_DEFAULT,
                               UINT(typedUav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE),
                               0,
                               0,
                               0};
        D3D11_SUBRESOURCE_DATA initial{storage.data(), 0, 0};
        check(device->CreateBuffer(&desc, &initial, &buffer), "Create precision reference storage");
        Com<ID3D11UnorderedAccessView> inputUav;
        if (typedUav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT(format);
            vd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            vd.Buffer = {1, 3, 0};
            check(device->CreateUnorderedAccessView(buffer.Get(), &vd, &inputUav),
                  "Create precision reference UAV");
        } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT(format);
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer = {1, 3};
            check(device->CreateShaderResourceView(buffer.Get(), &vd, &view),
                  "Create precision reference SRV");
        }
        const std::array<uint32_t, 4> values{index, 0, 0, 0};
        desc = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        initial.pSysMem = values.data();
        check(device->CreateBuffer(&desc, &initial, &constants), "Create precision reference constants");
        desc = {16,
                D3D11_USAGE_DEFAULT,
                UINT(stage == 1 ? D3D11_BIND_STREAM_OUTPUT : D3D11_BIND_UNORDERED_ACCESS),
                0,
                stage == 1 ? 0u : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                stage == 1 ? 0u : 16u};
        Com<ID3D11Buffer> resultBuffer;
        check(device->CreateBuffer(&desc, nullptr, &resultBuffer), "Create precision reference result");
        auto *cb = constants.Get();
        auto *srv = view.Get();
        if (stage == 1) {
            check(device->CreateVertexShader(code.data(), code.size(), nullptr, &vertex),
                  "Create precision reference VS");
            const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 0, 0, 4, 0};
            const UINT stride = 16, offset = 0;
            Com<ID3D11GeometryShader> stream;
            check(device->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), &entry, 1, &stride,
                                                               1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
                                                               &stream),
                  "Create precision reference SO");
            auto *streamTarget = resultBuffer.Get();
            context->VSSetShader(vertex.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, &cb);
            context->VSSetShaderResources(3, 1, &srv);
            context->GSSetShader(stream.Get(), nullptr, 0);
            context->SOSetTargets(1, &streamTarget, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->Draw(1, 0);
        } else {
            Com<ID3D11ComputeShader> shader;
            check(device->CreateComputeShader(code.data(), code.size(), nullptr, &shader),
                  "Create precision reference CS");
            Com<ID3D11UnorderedAccessView> resultView;
            check(device->CreateUnorderedAccessView(resultBuffer.Get(), nullptr, &resultView),
                  "Create precision result UAV");
            auto *uav = resultView.Get();
            auto *sourceUav = inputUav.Get();
            context->CSSetShader(shader.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, &cb);
            context->CSSetShaderResources(3, 1, &srv);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetUnorderedAccessViews(3, 1, &sourceUav, nullptr);
            context->Dispatch(1, 1, 1);
        }
        context->ClearState();
        const auto resultBytes = read(resultBuffer.Get());
        std::array<uint32_t, 4> result;
        std::memcpy(result.data(), resultBytes.data(), 16);
        validate();
        return result;
    }
    std::array<uint32_t, 4> run(Bytes code, Bytes storage, uint32_t format, uint32_t index,
                                uint32_t stage = 0, bool typedUav = false) {
        if (stage != 0)
            return runOtherStage(code, storage, format, index, stage, typedUav);
        const auto vs = compileHlsl("float4 main(uint id:SV_VertexID):SV_Position{"
                                    "if(id==0)return float4(-1,-1,0,1);if(id==1)return "
                                    "float4(-1,3,0,1);return float4(3,-1,0,1);}",
                                    "vs_5_0")
                            .bytecode;
        check(device->CreateVertexShader(vs.data(), vs.size(), nullptr, &vertex),
              "Create reference raster VS");
        check(device->CreatePixelShader(code.data(), code.size(), nullptr, &pixel), "Create reference PS");
        D3D11_BUFFER_DESC desc{
            UINT(storage.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA initial{storage.data(), 0, 0};
        check(device->CreateBuffer(&desc, &initial, &buffer), "Create reference typed pixel storage");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT(format);
        srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srv.Buffer = {1, 3};
        check(device->CreateShaderResourceView(buffer.Get(), &srv, &view),
              "Create reference typed pixel view");
        const std::array<uint32_t, 4> values{index, 0, 0, 0};
        desc = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        initial.pSysMem = values.data();
        check(device->CreateBuffer(&desc, &initial, &constants), "Create pixel reference constants");
        const D3D11_TEXTURE2D_DESC outputDesc{
            1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET,
            0, 0};
        check(device->CreateTexture2D(&outputDesc, nullptr, &output), "Create pixel reference output");
        check(device->CreateRenderTargetView(output.Get(), nullptr, &target),
              "Create pixel reference target");
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = rs.ScissorEnable = true;
        check(device->CreateRasterizerState(&rs, &rasterizer), "Create reference rasterizer");
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        check(device->CreateDepthStencilState(&ds, &depth), "Create reference depth state");
        D3D11_BLEND_DESC bs{};
        bs.RenderTarget[0] = {
            false,           D3D11_BLEND_ONE,  D3D11_BLEND_ZERO,   D3D11_BLEND_OP_ADD,
            D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        check(device->CreateBlendState(&bs, &blend), "Create reference blend state");
        bind();
        context->Draw(3, 0);
        context->ClearState();
        const auto result = readTarget();
        bind();
        validate();
        return result;
    }
};
std::vector<uint8_t> storage(uint32_t bytes) {
    const std::array<uint32_t, 4> special{1, 0x80000001, 0x7fc12345, 0x80000000};
    std::vector<uint8_t> result(bytes * 8);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = reinterpret_cast<const uint8_t *>(special.data())[i % 16];
    return result;
}
std::vector<uint8_t> shader(uint32_t format, uint32_t index, bool literal, bool partial,
                            const char *profile = "ps_5_0") {
    const auto info = *typedBufferFormat(format);
    const auto type = info.returnType == 5 ? "float" : info.returnType == 4 ? "uint" : "int";
    const auto address = literal ? std::to_string(index) + "u" : "p.x";
    const std::string expression = "asuint(src[" + address + "]" + (partial ? ".wy)" : ".wzyx)");
    return compileHlsl(std::string("cbuffer Params:register(b0){uint4 p;}Buffer<") + type +
                           "4>src:register(t3);uint4 main():" +
                           (std::string(profile) == "ps_5_0" ? "SV_Target" : "TEXCOORD0") + "{" +
                           (partial ? "uint4 x=uint4(17,23,31,47);x.xw=" + expression + ";return x;"
                                    : "return " + expression + ";") +
                           "}",
                       profile)
        .bytecode;
}
Inputs inputs(uint32_t format, uint32_t index, Buffer data, std::shared_ptr<GpuOperations> gpu) {
    const auto bytes = typedBufferFormat(format)->bytes;
    Inputs result;
    result.constantBuffers[0] = {Value::known({index, 0, 0, 0})};
    result.memory = std::make_shared<Memory>(
        std::map<uint64_t, Buffer>{{1, std::move(data)}},
        std::map<Register, BufferView>{{{7, {3}}, {1, bytes, bytes * 3, 0, {}, format}}}, 256 * 1024 * 1024,
        16 * 1024 * 1024, std::move(gpu));
    return result;
}
std::string hash(Bytes bytes) {
    return QCryptographicHash::hash(
               QByteArrayView(reinterpret_cast<const char *>(bytes.data()), bytes.size()),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}
std::vector<Step> execute(Lane &lane) {
    std::vector<Step> result;
    while (!lane.finished())
        result.push_back(lane.step());
    return result;
}
} // namespace
class DxbcTypedPixelTests final : public QObject {
    Q_OBJECT
    QTemporaryDir evidence;
  private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("FLORA_KEEP_TYPED_PRECISION_EVIDENCE")) {
            evidence.setAutoRemove(false);
            qInfo().noquote() << "Typed precision evidence:" << evidence.path();
        }
    }
    void minimumPrecision_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("stage");
        QTest::addColumn<uint32_t>("precision");
        QTest::addColumn<uint32_t>("addressPrecision");
        QTest::addColumn<bool>("literal");
        QTest::addColumn<bool>("partial");
        QTest::addColumn<bool>("typedUav");
        QTest::addColumn<bool>("refactoringAllowed");
        for (uint32_t vendor : {0u, 4318u, 32902u})
            for (uint32_t stage : {0u, 1u, 5u})
                for (uint32_t precision : {1u, 2u, 4u, 5u})
                    for (uint32_t addressPrecision : {0u, 4u, 5u})
                        for (bool literal : {false, true})
                            for (bool partial : {false, true})
                                for (bool typedUav : {false, true})
                                    for (bool refactoringAllowed : {false, true}) {
                                        if (typedUav && stage != 5)
                                            continue;
                                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5-%6-%7-%8")
                                                                     .arg(vendor)
                                                                     .arg(stage)
                                                                     .arg(precision)
                                                                     .arg(addressPrecision)
                                                                     .arg(literal)
                                                                     .arg(partial)
                                                                     .arg(typedUav)
                                                                     .arg(refactoringAllowed)))
                                            << vendor << stage << precision << addressPrecision << literal
                                            << partial << typedUav << refactoringAllowed;
                                    }
    }
    void minimumPrecision() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, stage);
        QFETCH(uint32_t, precision);
        QFETCH(uint32_t, addressPrecision);
        QFETCH(bool, literal);
        QFETCH(bool, partial);
        QFETCH(bool, typedUav);
        QFETCH(bool, refactoringAllowed);
        try {
            Device device(vendor);
            auto gpu = std::make_shared<GpuOperations>(device.device.Get(), vendor == 0);
            const std::string type = precision < 4 ? "float" : precision == 4 ? "int" : "uint";
            const auto minimumType = std::string(precision == 2 ? "min10" : "min16") + type;
            const auto format = typedUav         ? precision < 4    ? 41u
                                                   : precision == 4 ? 43u
                                                                    : 42u
                                : precision < 4  ? 2u
                                : precision == 4 ? 4u
                                                 : 3u;
            const auto stride = typedBufferFormat(format)->bytes;
            std::vector<uint8_t> storageBytes(stride * 8);
            for (size_t i = 0; i < storageBytes.size() / 4; ++i) {
                const uint32_t bits = precision < 4    ? std::bit_cast<uint32_t>(float(i) * .03127f - .271f)
                                      : precision == 4 ? uint32_t(int32_t(i) - 19)
                                                       : uint32_t(i * 17);
                std::memcpy(storageBytes.data() + i * 4, &bits, 4);
            }
            for (uint32_t index : {0u, 2u, 3u, 65536u, UINT32_MAX}) {
                const std::string addressType = addressPrecision == 4   ? "min16int"
                                                : addressPrecision == 5 ? "min16uint"
                                                                        : "uint";
                const std::string address = literal ? std::to_string(index) + "u" : "p.x";
                std::string body = addressType + " index=" + addressType + "(" + address + ")+" +
                                   addressType + "(p.y)*" + addressType + "(3);";
                body += minimumType + "4 value=src[index];uint4 result=uint4(17,23,31,47);";
                const std::string widened = precision < 4    ? "asuint(float4(value))"
                                            : precision == 4 ? "asuint(int4(value))"
                                                             : "uint4(value)";
                body += partial ? "result.xw=" + widened + ".wy;" : "result=" + widened + ".wzyx;";
                const auto source =
                    "cbuffer Params:register(b0){uint4 p;}" +
                    std::string(typedUav ? "RWBuffer<" : "Buffer<") + minimumType + "4>src:register(" +
                    (typedUav ? "u3" : "t3") + ");" +
                    (stage == 5
                         ? "RWStructuredBuffer<uint4> output:register(u0);[numthreads(1,1,1)]void main(){"
                     : stage == 1 ? "uint4 main():TEXCOORD0{"
                                  : "uint4 main():SV_Target{") +
                    body + (stage == 5 ? "output[0]=result;}" : "return result;}");
                const auto compiled = compileHlsl(source, stage == 5   ? "cs_5_0"
                                                          : stage == 1 ? "vs_5_0"
                                                                       : "ps_5_0")
                                          .bytecode;
                auto parts = readDxbcParts(compiled);
                std::vector<uint8_t> programBytes;
                for (auto &[tag, bytes] : parts)
                    if (tag == 0x58454853 || tag == 0x52444853) {
                        auto program = readDxbcProgram(bytes);
                        for (auto &row : program.instructions)
                            if ((row[0] & 2047) == 106)
                                row[0] = (row[0] & ~(1u << 11)) | (refactoringAllowed ? 1u << 11 : 0u);
                        programBytes = writeDxbcProgram(program);
                        bytes = programBytes;
                    }
                const auto code = makeDxbc(parts);
                const Program program(code);
                unsigned loads = 0;
                for (const auto &instruction : program.instructions()) {
                    if (instruction.opcode != (typedUav ? 163u : 45u))
                        continue;
                    ++loads;
                    const auto operands = checkpoint::instructionOperands(instruction.words);
                    const auto first = operands.ranges.at(0).first;
                    QCOMPARE(instruction.words[first] >> 31, 1u);
                    QCOMPARE((instruction.words[first + 1] >> 14) & 7, precision);
                    const auto addressFirst = operands.ranges.at(1).first;
                    QCOMPARE(instruction.words[addressFirst] >> 31
                                 ? (instruction.words[addressFirst + 1] >> 14) & 7
                                 : 0u,
                             addressPrecision);
                }
                QCOMPARE(loads, 1u);
                const auto reference = device.run(code, storageBytes, format, index, stage, typedUav);
                auto input = inputs(format, index, Buffer::known(storageBytes), gpu);
                input.gpu = gpu;
                if (stage == 5) {
                    input.memory = std::make_shared<Memory>(
                        std::map<uint64_t, Buffer>{{1, Buffer::known(storageBytes)},
                                                   {2, Buffer::known(std::vector<uint8_t>(16))}},
                        std::map<Register, BufferView>{
                            {{typedUav ? 30u : 7u, {3}}, {1, stride, stride * 3, 0, {}, format}},
                            {{30, {0}}, {2, 0, 16, 16}}},
                        256 * 1024 * 1024, 16 * 1024 * 1024, gpu);
                }
                Lane lane(program, input);
                const auto trace = execute(lane);
                auto actual = lane.value({2, {0}});
                if (stage == 5) {
                    const auto &output = input.memory->buffers().at(2);
                    std::memcpy(actual.bits.data(), output.bytes.data(), 16);
                    actual.valid = 0;
                    for (unsigned c = 0; c < 4; ++c)
                        if (std::all_of(output.valid.begin() + c * 4, output.valid.begin() + c * 4 + 4,
                                        [](auto b) { return b != 0; }))
                            actual.valid |= uint8_t(1u << c);
                }
                const auto name = QString::fromLatin1(QTest::currentDataTag()) + "-" + QString::number(index);
                nlohmann::json report{{"probe", "typed-load-precision"},
                                      {"vendor", vendor},
                                      {"stage", stage},
                                      {"precision", precision},
                                      {"address_precision", addressPrecision},
                                      {"literal", literal},
                                      {"partial", partial},
                                      {"typed_uav", typedUav},
                                      {"refactoring_allowed", refactoringAllowed},
                                      {"format", format},
                                      {"index", index},
                                      {"shader_sha256", hash(code)},
                                      {"compiler_sha256", hash(compiled)},
                                      {"reference_adapter",
                                       {{"vendor_id", device.adapterIdentity.VendorId},
                                        {"device_id", device.adapterIdentity.DeviceId},
                                        {"luid_low", device.adapterIdentity.AdapterLuid.LowPart},
                                        {"luid_high", device.adapterIdentity.AdapterLuid.HighPart}}},
                                      {"storage_sha256", hash(storageBytes)},
                                      {"cpu", actual.bits},
                                      {"gpu", reference},
                                      {"valid", actual.valid},
                                      {"helper", gpu->identity()}};
                report["steps"] = nlohmann::json::array();
                for (const auto &step : trace) {
                    nlohmann::json changes = nlohmann::json::array();
                    for (const auto &change : step.changes)
                        changes.push_back({{"kind", change.destination.kind},
                                           {"indices", change.destination.indices},
                                           {"bits", change.after.bits},
                                           {"valid", change.after.valid}});
                    report["steps"].push_back({{"opcode", program.instructions().at(step.instruction).opcode},
                                               {"word", step.wordOffset},
                                               {"changes", changes}});
                }
                for (const auto &[suffix, bytes] : std::vector<std::pair<QString, std::vector<uint8_t>>>{
                         {".dxbc", code}, {".compiler.dxbc", compiled}, {".bin", storageBytes}}) {
                    QFile file(evidence.filePath(name + suffix));
                    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                    QCOMPARE(file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())),
                             qint64(bytes.size()));
                }
                QFile reportFile(evidence.filePath(name + ".json"));
                QVERIFY(reportFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
                const auto text = report.dump(2);
                QCOMPARE(reportFile.write(text.data(), qint64(text.size())), qint64(text.size()));
                qInfo().noquote() << QString::fromStdString(report.dump());
                QCOMPARE(actual.bits, reference);
                QCOMPARE(actual.valid, uint8_t(15));
                QCOMPARE(input.memory->buffers().at(1).bytes, storageBytes);
                QCOMPARE(device.read(device.buffer.Get()), storageBytes);
                if (stage == 0)
                    QVERIFY(device.bindingsPreserved());
                device.validate();
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void minimumPrecisionValidityAndCancellation() {
        try {
            Device device(0);
            bool cancelled = false;
            auto gpu = std::make_shared<GpuOperations>(device.device.Get(), true, [&] { return cancelled; });
            const auto code =
                compileHlsl("cbuffer Params:register(b0){uint4 p;}Buffer<min16float4> src:register(t3);"
                            "uint4 main():SV_Target{min16uint index=min16uint(p.x)+min16uint(p.y);"
                            "return asuint(float4(src[index]));}",
                            "ps_5_0")
                    .bytecode;
            const Program program(code);
            auto bytes = Buffer::known(storage(16));
            bytes.valid[16 + 4] = 0;
            auto input = inputs(2, 0, bytes, gpu);
            input.gpu = gpu;
            Lane lane(program, input);
            cancelled = true;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, execute(lane));
            QCOMPARE(gpu->operations(), uint64_t(0));
            cancelled = false;
            execute(lane);
            QCOMPARE(lane.value({2, {0}}).valid, uint8_t(13));
            QCOMPARE(input.memory->buffers().at(1).bytes, bytes.bytes);
            QCOMPARE(input.memory->buffers().at(1).valid, bytes.valid);
            auto unknown = input;
            unknown.constantBuffers[0][0].valid = 0;
            Lane unknownAddress(program, unknown);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(unknownAddress));
            QCOMPARE(input.memory->buffers().at(1).bytes, bytes.bytes);
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void hardware_data() {
        QTest::addColumn<uint32_t>("vendor");
        QTest::addColumn<uint32_t>("format");
        QTest::addColumn<uint32_t>("index");
        QTest::addColumn<bool>("literal");
        QTest::addColumn<bool>("partial");
        for (uint32_t vendor : {0u, 0x10deu, 0x8086u})
            for (uint32_t format : {41u, 42u, 43u, 2u, 11u, 31u, 30u, 50u, 62u, 24u, 26u, 6u})
                for (uint32_t index : {0u, 2u, 3u, UINT32_MAX})
                    for (bool literal : {false, true})
                        for (bool partial : {false, true})
                            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5")
                                                         .arg(vendor)
                                                         .arg(format)
                                                         .arg(index)
                                                         .arg(literal)
                                                         .arg(partial)))
                                << vendor << format << index << literal << partial;
    }
    void hardware() {
        QFETCH(uint32_t, vendor);
        QFETCH(uint32_t, format);
        QFETCH(uint32_t, index);
        QFETCH(bool, literal);
        QFETCH(bool, partial);
        try {
            Device device(vendor);
            const auto code = shader(format, index, literal, partial);
            const auto bytes = storage(typedBufferFormat(format)->bytes);
            const auto reference = device.run(code, bytes, format, index);
            auto gpu = std::make_shared<GpuOperations>(device.device.Get(), !vendor);
            const auto input = inputs(format, index, Buffer::known(bytes), gpu);
            Program program(code);
            QCOMPARE(program.stage(), 0u);
            Lane lane(program, input);
            const auto trace = execute(lane);
            const auto cpu = lane.value({2, {0}});
            qInfo().noquote() << QString::fromStdString(nlohmann::json({{"probe", "dxbc-typed-pixel"},
                                                                        {"vendor", vendor},
                                                                        {"format", format},
                                                                        {"index", index},
                                                                        {"literal", literal},
                                                                        {"partial", partial},
                                                                        {"shader_sha256", hash(code)},
                                                                        {"storage_sha256", hash(bytes)},
                                                                        {"cpu", cpu.bits},
                                                                        {"gpu", reference},
                                                                        {"valid", cpu.valid},
                                                                        {"helper", gpu->identity()}})
                                                            .dump());
            QCOMPARE(cpu.bits, reference);
            QCOMPARE(cpu.valid, uint8_t(15));
            QCOMPARE(gpu->operations(), uint64_t(1));
            QCOMPARE(
                std::count_if(trace.begin(), trace.end(), [](const Step &s) { return s.memoryGpuAssisted; }),
                1);
            auto reversed = lane.registers();
            for (auto step = trace.rbegin(); step != trace.rend(); ++step) {
                QVERIFY(step->memoryChanges.empty());
                for (auto change = step->changes.rbegin(); change != step->changes.rend(); ++change)
                    reversed[change->destination] = change->before;
            }
            for (const auto &[reg, value] : reversed) {
                QCOMPARE(value.valid, uint8_t(0));
                QCOMPARE(value.bits, (std::array<uint32_t, 4>{}));
            }
            QCOMPARE(input.memory->buffers().at(1).bytes, bytes);
            QVERIFY(device.bindingsPreserved());
            QCOMPARE(device.read(device.buffer.Get()), bytes);
            QCOMPARE(device.readTarget(), reference);
            device.validate();
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void validityAndCancellation() {
        try {
            Device device(0);
            bool cancelled = false;
            auto gpu = std::make_shared<GpuOperations>(device.device.Get(), true, [&] { return cancelled; });
            auto bytes = Buffer::known(storage(16));
            bytes.valid[16 + 4] = 0;
            const auto input = inputs(2, 0, bytes, gpu);
            Program program(shader(2, 0, false, true));
            Lane lane(program, input);
            cancelled = true;
            QVERIFY_THROWS_EXCEPTION(OperationCancelled, execute(lane));
            QCOMPARE(gpu->operations(), uint64_t(0));
            cancelled = false;
            execute(lane);
            QCOMPARE(lane.value({2, {0}}).valid, uint8_t(7));
            QCOMPARE(gpu->operations(), uint64_t(1));
            QCOMPARE(input.memory->buffers().at(1).bytes, bytes.bytes);
            QCOMPARE(input.memory->buffers().at(1).valid, bytes.valid);
            auto unknown = input;
            unknown.constantBuffers[0][0].valid = 0;
            Lane unavailableAddress(program, unknown);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, execute(unavailableAddress));
            QCOMPARE(gpu->operations(), uint64_t(1));
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
    void missingAndUnbound() {
        try {
            Program program(shader(42, 0, false, false));
            for (bool unbound : {false, true}) {
                Inputs input;
                input.constantBuffers[0] = {Value::known({})};
                std::map<Register, BufferView> views;
                if (unbound)
                    views[{7, {3}}] = {};
                input.memory = std::make_shared<Memory>(std::map<uint64_t, Buffer>{}, views);
                Lane lane(program, input);
                execute(lane);
                QCOMPARE(lane.value({2, {0}}).valid, uint8_t(unbound ? 15 : 0));
                QCOMPARE(lane.value({2, {0}}).bits, (std::array<uint32_t, 4>{}));
            }
        } catch (const std::exception &error) {
            QFAIL(error.what());
        }
    }
};
QTEST_GUILESS_MAIN(DxbcTypedPixelTests)
#include "DxbcTypedPixelTests.moc"
