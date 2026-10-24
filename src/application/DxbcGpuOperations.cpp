#include "DxbcGpuOperations.h"
#include "DxbcCheckpointModel.h"
#include "DxbcTextureInputs.h"
#include "DxbcTypedBuffer.h"
#include "HlslCompilation.h"
#include "core/Dxbc.h"
#include "core/TextureStorage.h"
#include "replay/Replay.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <thread>

namespace flora::dxbc_exec {
namespace {
using Words = std::vector<uint32_t>;
Words row(uint32_t opcode, std::initializer_list<Words> args) {
    Words result{opcode};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words dest(uint32_t kind, uint32_t index, uint32_t mask = 15) {
    return {0x100002 | (kind << 12) | (mask << 4), index};
}
Words source(uint32_t kind, uint32_t index, uint32_t swizzle = 0xe4) {
    return {0x100006 | (kind << 12) | (swizzle << 4), index};
}
Words immediate(uint32_t value) { return {0x4001, value}; }
DXGI_ADAPTER_DESC adapter(ID3D11Device *device, Com<IDXGIAdapter> &object) {
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Get memory assist DXGI device");
    check(dxgi->GetAdapter(&object), "Get memory assist adapter");
    DXGI_ADAPTER_DESC result{};
    check(object->GetDesc(&result), "Get memory assist adapter identity");
    return result;
}
Words kernel(bool store, uint32_t stride, uint32_t kind, uint32_t mask, uint32_t swizzle, uint32_t index,
             uint32_t offset, bool literalIndex, bool literalOffset) {
    std::vector<Words> rows{row(89, {{0x208e46, 0, 2}}), row(104, {{1}}), row(155, {{1, 1, 1}})};
    const auto declaration = kind == 7 ? (stride ? 162u : 161u) : (stride ? 158u : 157u);
    rows.push_back(stride ? row(declaration, {dest(kind, 0), {stride}}) : row(declaration, {dest(kind, 0)}));
    if (!store)
        rows.push_back(row(158, {dest(30, 1), {16}}));
    const Words element = literalIndex ? immediate(index) : Words{0x20800a, 0, 0};
    const Words field = literalOffset ? immediate(offset) : Words{0x20801a, 0, 0};
    if (store) {
        const Words value{0x208e46, 0, 1};
        rows.push_back(stride ? row(168, {dest(30, 0, mask), element, field, value})
                              : row(166, {dest(30, 0, mask), field, value}));
    } else {
        rows.push_back(row(54, {dest(0, 0), immediate(0)}));
        rows.push_back(stride ? row(167, {dest(0, 0, mask), element, field, source(kind, 0, swizzle)})
                              : row(165, {dest(0, 0, mask), field, source(kind, 0, swizzle)}));
        rows.push_back(row(168, {dest(30, 1), immediate(0), immediate(0), source(0, 0)}));
    }
    rows.push_back(row(62, {}));
    const auto code = writeDxbcProgram({{0x50050, 0}, rows});
    Words result(code.size() / 4);
    std::memcpy(result.data(), code.data(), code.size());
    return result;
}
Words typedKernel(bool store, uint32_t kind, uint32_t mask, uint32_t swizzle, uint32_t index, bool literal,
                  uint32_t returns) {
    std::vector<Words> rows{row(89, {{0x208e46, 0, 2}}), row(104, {{1}}), row(155, {{1, 1, 1}}),
                            row((kind == 7 ? 88u : 156u) | 0x800, {dest(kind, 0), {returns}})};
    const Words element = literal ? immediate(index) : Words{0x20800a, 0, 0};
    if (store)
        rows.push_back(row(164, {dest(30, 0), element, {0x208e46, 0, 1}}));
    else {
        rows.push_back(row(158, {dest(30, 1), {16}}));
        rows.push_back(row(54, {dest(0, 0), immediate(0)}));
        rows.push_back(row(kind == 7 ? 45u : 163u, {dest(0, 0, mask), element, source(kind, 0, swizzle)}));
        rows.push_back(row(168, {dest(30, 1), immediate(0), immediate(0), source(0, 0)}));
    }
    rows.push_back(row(62, {}));
    const auto bytes = writeDxbcProgram({{0x50050, 0}, rows});
    Words result(bytes.size() / 4);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
Words typedGraphicsKernel(uint32_t stage, uint32_t mask, uint32_t swizzle, uint32_t index, bool literal,
                          uint32_t returns) {
    const Words address = literal ? immediate(index) : Words{0x20800a, 0, 0};
    const auto code =
        writeDxbcProgram({{(stage << 16) | 0x50, 0},
                          {row(89, {{0x208e46, 0, 1}}), row(88 | 0x800, {dest(7, 0), {returns}}),
                           row(101, {dest(2, 0)}), row(54, {dest(2, 0), immediate(0)}),
                           row(45, {dest(2, 0, mask), address, source(7, 0, swizzle)}), row(62, {})}});
    Words result(code.size() / 4);
    std::memcpy(result.data(), code.data(), code.size());
    return result;
}
Words textureKernel(uint32_t stage, uint32_t dimension, uint32_t returns, uint32_t mask, uint32_t swizzle,
                    const std::array<int32_t, 3> &offset) {
    std::vector<Words> rows{row(89, {{0x208e46, 0, 1}}),
                            row(88 | (dimension << 11), {dest(7, 0), {returns}})};
    const bool compute = stage == 5;
    if (compute) {
        rows.push_back(row(104, {{1}}));
        rows.push_back(row(155, {{1, 1, 1}}));
        rows.push_back(row(158, {dest(30, 0), {16}}));
    } else
        rows.push_back(row(101, {dest(2, 0)}));
    const auto target = compute ? 0u : 2u;
    rows.push_back(row(54, {dest(target, 0), immediate(0)}));
    uint32_t control = 1;
    for (unsigned c = 0; c < 3; ++c) {
        if (offset[c] < -8 || offset[c] > 7)
            throw std::runtime_error("Native texture offset exceeds the DXBC immediate range");
        control |= (uint32_t(offset[c]) & 15) << (9 + c * 4);
    }
    rows.push_back(
        row(45 | 0x80000000, {{control}, dest(target, 0, mask), {0x208e46, 0, 0}, source(7, 0, swizzle)}));
    if (compute)
        rows.push_back(row(168, {dest(30, 0), immediate(0), immediate(0), source(0, 0)}));
    rows.push_back(row(62, {}));
    const auto bytes = writeDxbcProgram({{(stage << 16) | 0x50, 0}, rows});
    Words result(bytes.size() / 4);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
} // namespace
struct GpuOperations::State {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    std::map<Words, Com<ID3D11ComputeShader>> shaders;
    std::map<Words, Value> textureDimensionResults;
    std::map<Words, std::optional<uint64_t>> textureFootprints;
    struct TextureUavOperation {
        Com<ID3D11Resource> texture, staging;
        Com<ID3D11UnorderedAccessView> view;
        std::vector<uint8_t> shadow;
    };
    std::map<Words, TextureUavOperation> textureUavs;
    uint64_t textureUavBytes = 0;
    Com<ID3D11Buffer> textureUavConstants, textureUavOutput, textureUavStaging;
    Com<ID3D11UnorderedAccessView> textureUavOutputView;
    struct VertexOperation {
        Com<ID3D11VertexShader> vertex;
        Com<ID3D11GeometryShader> stream;
    };
    std::map<Words, VertexOperation> vertexShaders;
    std::map<Words, Com<ID3D11PixelShader>> pixelShaders;
    Com<ID3D11VertexShader> pixelVertex;
    Com<ID3D11RasterizerState> pixelRasterizer;
    DXGI_ADAPTER_DESC description{};
    LARGE_INTEGER driver{};
    CancelCheck cancelled;
    uint32_t timeoutMs;
    uint64_t count = 0;
    bool failed = false, warp = false, debug = false;
    Value vertex(const Words &words, const std::vector<Value> &constants,
                 ID3D11ShaderResourceView *resource = nullptr) {
        checkCancellation(cancelled);
        if (failed)
            throw std::runtime_error("Recreate the native GPU assist after a failed operation");
        try {
            auto found = vertexShaders.find(words);
            if (found == vertexShaders.end()) {
                if (vertexShaders.size() >= 256)
                    vertexShaders.clear();
                const auto signature = compileHlsl("uint4 main():TEXCOORD0{return 0;}", "vs_5_0").bytecode;
                auto parts = readDxbcParts(signature);
                std::erase_if(parts, [](const auto &part) {
                    return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
                });
                std::vector<uint8_t> bytes(words.size() * 4);
                std::memcpy(bytes.data(), words.data(), bytes.size());
                parts.emplace_back(0x58454853, bytes);
                const auto binary = makeDxbc(parts);
                VertexOperation operation;
                check(device->CreateVertexShader(binary.data(), binary.size(), nullptr, &operation.vertex),
                      "Create native instruction vertex shader");
                const D3D11_SO_DECLARATION_ENTRY entry{0, "TEXCOORD", 0, 0, 4, 0};
                const UINT stride = 16;
                check(device->CreateGeometryShaderWithStreamOutput(binary.data(), binary.size(), &entry, 1,
                                                                   &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM,
                                                                   nullptr, &operation.stream),
                      "Create native instruction stream output");
                found = vertexShaders.emplace(words, std::move(operation)).first;
            }
            std::vector<std::array<uint32_t, 4>> bits;
            for (const auto &value : constants)
                bits.push_back(value.bits);
            D3D11_BUFFER_DESC desc{
                UINT(bits.size() * 16), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA initial{bits.data(), 0, 0};
            Com<ID3D11Buffer> cb, output, staging;
            check(device->CreateBuffer(&desc, &initial, &cb), "Create native instruction constants");
            desc = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
            check(device->CreateBuffer(&desc, nullptr, &output), "Create native instruction output");
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateBuffer(&desc, nullptr, &staging), "Create native instruction staging");
            auto *constant = cb.Get();
            auto *target = output.Get();
            const UINT offset = 0;
            context->VSSetShader(found->second.vertex.Get(), nullptr, 0);
            context->VSSetConstantBuffers(0, 1, &constant);
            context->VSSetShaderResources(0, 1, &resource);
            context->GSSetShader(found->second.stream.Get(), nullptr, 0);
            context->SOSetTargets(1, &target, &offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->Draw(1, 0);
            context->ClearState();
            context->CopyResource(staging.Get(), output.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            for (;;) {
                checkCancellation(cancelled);
                const auto hr =
                    context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == S_OK)
                    break;
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                    check(hr, "Read native instruction result");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Native instruction operation timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Value result;
            std::memcpy(result.bits.data(), mapped.pData, 16);
            context->Unmap(staging.Get(), 0);
            if (debug) {
                Com<ID3D11InfoQueue> queue;
                check(device.As(&queue), "Read native instruction validation queue");
                for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T size = 0;
                    check(queue->GetMessage(i, nullptr, &size), "Read instruction validation message size");
                    std::vector<uint8_t> data(size);
                    auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                    check(queue->GetMessage(i, message, &size), "Read instruction validation message");
                    if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(message->pDescription);
                }
                queue->ClearStoredMessages();
            }
            ++count;
            return result;
        } catch (...) {
            context->ClearState();
            failed = true;
            throw;
        }
    }
    Value pixel(const Words &words, const std::array<uint32_t, 4> &values,
                ID3D11ShaderResourceView *resource) {
        checkCancellation(cancelled);
        if (failed)
            throw std::runtime_error("Recreate the native GPU assist after a failed operation");
        try {
            if (!pixelVertex) {
                const auto code =
                    compileHlsl(
                        "float4 main(uint id:SV_VertexID):SV_Position{float2 uv=float2((id<<1)&2,id&2);"
                        "return float4(uv*float2(2,-2)+float2(-1,1),0,1);}",
                        "vs_5_0")
                        .bytecode;
                check(device->CreateVertexShader(code.data(), code.size(), nullptr, &pixelVertex),
                      "Create native pixel assist vertex shader");
                D3D11_RASTERIZER_DESC desc{};
                desc.FillMode = D3D11_FILL_SOLID;
                desc.CullMode = D3D11_CULL_NONE;
                desc.DepthClipEnable = true;
                check(device->CreateRasterizerState(&desc, &pixelRasterizer),
                      "Create native pixel rasterizer");
            }
            auto found = pixelShaders.find(words);
            if (found == pixelShaders.end()) {
                if (pixelShaders.size() >= 256)
                    pixelShaders.clear();
                // DxbcParts borrows chunk bytes; keep both owners alive through makeDxbc.
                const auto signature = compileHlsl("uint4 main():SV_Target0{return 0;}", "ps_5_0").bytecode;
                auto parts = readDxbcParts(signature);
                std::erase_if(parts, [](const auto &part) {
                    return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
                });
                std::vector<uint8_t> bytes(words.size() * 4);
                std::memcpy(bytes.data(), words.data(), bytes.size());
                parts.emplace_back(0x58454853, bytes);
                const auto code = makeDxbc(parts);
                Com<ID3D11PixelShader> shader;
                check(device->CreatePixelShader(code.data(), code.size(), nullptr, &shader),
                      "Create native pixel operation");
                found = pixelShaders.emplace(words, std::move(shader)).first;
            }
            const D3D11_BUFFER_DESC cbDesc{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            const D3D11_SUBRESOURCE_DATA initial{values.data(), 0, 0};
            Com<ID3D11Buffer> constants;
            check(device->CreateBuffer(&cbDesc, &initial, &constants), "Create native pixel constants");
            D3D11_TEXTURE2D_DESC desc{1,
                                      1,
                                      1,
                                      1,
                                      DXGI_FORMAT_R32G32B32A32_UINT,
                                      {1, 0},
                                      D3D11_USAGE_DEFAULT,
                                      D3D11_BIND_RENDER_TARGET,
                                      0,
                                      0};
            Com<ID3D11Texture2D> output, staging;
            check(device->CreateTexture2D(&desc, nullptr, &output), "Create native pixel output");
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(device->CreateTexture2D(&desc, nullptr, &staging), "Create native pixel readback");
            Com<ID3D11RenderTargetView> target;
            check(device->CreateRenderTargetView(output.Get(), nullptr, &target),
                  "Create native pixel target");
            const D3D11_QUERY_DESC queryDesc{D3D11_QUERY_OCCLUSION, 0};
            Com<ID3D11Query> coverage;
            check(device->CreateQuery(&queryDesc, &coverage), "Create native pixel coverage query");
            auto *cb = constants.Get();
            auto *rtv = target.Get();
            const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
            context->VSSetShader(pixelVertex.Get(), nullptr, 0);
            context->PSSetShader(found->second.Get(), nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &cb);
            context->PSSetShaderResources(0, 1, &resource);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->RSSetState(pixelRasterizer.Get());
            context->RSSetViewports(1, &viewport);
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->Begin(coverage.Get());
            context->Draw(3, 0);
            context->End(coverage.Get());
            context->ClearState();
            context->CopyResource(staging.Get(), output.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            for (;;) {
                checkCancellation(cancelled);
                const auto hr =
                    context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == S_OK)
                    break;
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                    check(hr, "Read native pixel result");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Native pixel operation timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Value result;
            std::memcpy(result.bits.data(), mapped.pData, 16);
            context->Unmap(staging.Get(), 0);
            UINT64 samples = 0;
            for (;;) {
                checkCancellation(cancelled);
                const auto hr = context->GetData(coverage.Get(), &samples, sizeof samples,
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hr == S_OK)
                    break;
                check(hr, "Read native pixel coverage");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Native pixel coverage timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (samples != 1)
                throw std::runtime_error("Native pixel assist did not cover exactly one sample");
            if (debug) {
                Com<ID3D11InfoQueue> queue;
                check(device.As(&queue), "Read native pixel validation queue");
                for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T size = 0;
                    check(queue->GetMessage(i, nullptr, &size), "Read pixel validation size");
                    std::vector<uint8_t> bytes(size);
                    auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                    check(queue->GetMessage(i, message, &size), "Read pixel validation message");
                    if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(message->pDescription);
                }
                queue->ClearStoredMessages();
            }
            ++count;
            return result;
        } catch (...) {
            context->ClearState();
            failed = true;
            throw;
        }
    }
    Value typedGraphics(uint32_t stage, const Buffer &buffer, const BufferView &view, uint32_t index,
                        uint32_t returns, uint32_t swizzle, uint32_t mask, bool literal) {
        checkCancellation(cancelled);
        if (failed)
            throw std::runtime_error("Recreate the native memory assist after a failed operation");
        const auto format = typedBufferFormat(view.format);
        if (!format || !view.resource || !view.size || buffer.bytes.empty() ||
            buffer.bytes.size() > 256ull * 1024 * 1024 || view.offset > buffer.bytes.size() ||
            view.size > buffer.bytes.size() - view.offset || view.offset % format->bytes ||
            view.size % format->bytes)
            throw std::runtime_error("Invalid native graphics buffer storage or view");
        try {
            UINT support = 0;
            check(device->CheckFormatSupport(DXGI_FORMAT(view.format), &support),
                  "Check graphics buffer format support");
            constexpr auto required = D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
            if ((support & required) != required)
                throw std::runtime_error("Selected device does not support this graphics buffer format");
            const D3D11_BUFFER_DESC desc{
                UINT(buffer.bytes.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0};
            const D3D11_SUBRESOURCE_DATA initial{buffer.bytes.data(), 0, 0};
            Com<ID3D11Buffer> resource;
            check(device->CreateBuffer(&desc, &initial, &resource),
                  "Create private graphics buffer snapshot");
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT(view.format);
            srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srv.Buffer = {UINT(view.offset / format->bytes), UINT(view.size / format->bytes)};
            Com<ID3D11ShaderResourceView> native;
            check(device->CreateShaderResourceView(resource.Get(), &srv, &native),
                  "Create private graphics buffer view");
            const auto words = typedGraphicsKernel(stage, mask, swizzle, index, literal, returns);
            if (stage == 0)
                return pixel(words, {index, 0, 0, 0}, native.Get());
            return vertex(words, {Value::known({index, 0, 0, 0})}, native.Get());
        } catch (...) {
            context->ClearState();
            failed = true;
            throw;
        }
    }
    Value texture(uint32_t stage, const TextureInput &input, const TextureInputView &view, uint32_t dimension,
                  uint32_t returns, const Value &address, uint32_t swizzle, uint32_t mask,
                  const std::array<int32_t, 3> &offset) {
        checkCancellation(cancelled);
        if (failed)
            throw std::runtime_error("Recreate the native GPU assist after a failed operation");
        if ((stage != 0 && stage != 1 && stage != 5) || !view.resource || input.storage.bytes.empty() ||
            input.storage.bytes.size() > 256ull * 1024 * 1024 || view.nativeDescriptor.size() != 6 ||
            (view.planeSlice && *view.planeSlice))
            throw std::runtime_error("Invalid or unsupported native texture load request");
        try {
            auto r = input.resource;
            auto &d = r.desc;
            d.at(d.size() - 4) = D3D11_USAGE_IMMUTABLE;
            d.at(d.size() - 3) = D3D11_BIND_SHADER_RESOURCE;
            d.at(d.size() - 2) = 0;
            d.back() &= D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
            const auto layout = textureInitialSubresources(r, input.storage.bytes);
            std::vector<D3D11_SUBRESOURCE_DATA> initial;
            for (const auto &sub : layout)
                initial.push_back(
                    {input.storage.bytes.data() + sub.offset, sub.rowPitch, UINT(sub.slicePitch)});
            Com<ID3D11Resource> resource;
            auto create = [&]<class T, class D>(auto method) {
                if (d.size() * 4 != sizeof(D))
                    throw std::runtime_error("Native texture descriptor size mismatch");
                D descriptor{};
                std::memcpy(&descriptor, d.data(), sizeof descriptor);
                Com<T> value;
                check((device.Get()->*method)(&descriptor, initial.data(), &value),
                      "Create private texture snapshot");
                resource = value;
            };
            if (r.type == 0x84)
                create.operator()<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>(&ID3D11Device::CreateTexture1D);
            else if (r.type == 0x85)
                create.operator()<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>(&ID3D11Device::CreateTexture2D);
            else if (r.type == 0x86)
                create.operator()<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>(&ID3D11Device::CreateTexture3D);
            else
                throw std::runtime_error("Unsupported native texture storage type");
            if (hasResourceLodClamp(r))
                context->SetResourceMinLOD(resource.Get(), input.minLod);
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            std::memcpy(&desc, view.nativeDescriptor.data(), sizeof desc);
            if (uint32_t(desc.Format) != view.format || uint32_t(desc.ViewDimension) != view.dimension)
                throw std::runtime_error("Native texture descriptor and view metadata disagree");
            Com<ID3D11ShaderResourceView> srv;
            check(device->CreateShaderResourceView(resource.Get(), &desc, &srv),
                  "Create private texture view");
            const auto words = textureKernel(stage, dimension, returns, mask, swizzle, offset);
            if (stage == 0)
                return pixel(words, address.bits, srv.Get());
            if (stage == 1)
                return vertex(words, {address}, srv.Get());
            auto found = shaders.find(words);
            if (found == shaders.end()) {
                if (shaders.size() >= 256)
                    shaders.clear();
                const auto signature = compileHlsl("[numthreads(1,1,1)]void main(){}", "cs_5_0").bytecode;
                auto parts = readDxbcParts(signature);
                std::erase_if(parts, [](const auto &part) {
                    return part.first == 0x58454853 || part.first == 0x52444853 || part.first == 0x54415453;
                });
                std::vector<uint8_t> bytes(words.size() * 4);
                std::memcpy(bytes.data(), words.data(), bytes.size());
                parts.emplace_back(0x58454853, bytes);
                const auto code = makeDxbc(parts);
                Com<ID3D11ComputeShader> shader;
                check(device->CreateComputeShader(code.data(), code.size(), nullptr, &shader),
                      "Create native texture load shader");
                found = shaders.emplace(words, std::move(shader)).first;
            }
            D3D11_BUFFER_DESC bd{16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA cbData{address.bits.data(), 0, 0};
            Com<ID3D11Buffer> cb, output, staging;
            check(device->CreateBuffer(&bd, &cbData, &cb), "Create texture load constants");
            bd = {16,
                  D3D11_USAGE_DEFAULT,
                  D3D11_BIND_UNORDERED_ACCESS,
                  0,
                  D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                  16};
            check(device->CreateBuffer(&bd, nullptr, &output), "Create texture load output");
            Com<ID3D11UnorderedAccessView> target;
            check(device->CreateUnorderedAccessView(output.Get(), nullptr, &target),
                  "Create texture load output view");
            bd = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            check(device->CreateBuffer(&bd, nullptr, &staging), "Create texture load staging");
            auto *constants = cb.Get();
            auto *source = srv.Get();
            auto *destination = target.Get();
            context->CSSetShader(found->second.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, &constants);
            context->CSSetShaderResources(0, 1, &source);
            context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
            context->Dispatch(1, 1, 1);
            context->ClearState();
            context->CopyResource(staging.Get(), output.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            for (;;) {
                checkCancellation(cancelled);
                const auto hr =
                    context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == S_OK)
                    break;
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                    check(hr, "Read native texture load result");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Native texture load timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Value result;
            std::memcpy(result.bits.data(), mapped.pData, 16);
            context->Unmap(staging.Get(), 0);
            if (debug) {
                Com<ID3D11InfoQueue> queue;
                check(device.As(&queue), "Read texture load validation queue");
                for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T size = 0;
                    check(queue->GetMessage(i, nullptr, &size), "Read texture load validation size");
                    std::vector<uint8_t> bytes(size);
                    auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
                    check(queue->GetMessage(i, message, &size), "Read texture load validation message");
                    if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(message->pDescription);
                }
                queue->ClearStoredMessages();
            }
            ++count;
            return result;
        } catch (...) {
            context->ClearState();
            failed = true;
            throw;
        }
    }
    std::vector<uint8_t> execute(const Buffer &buffer, const BufferView &view, bool store, uint32_t kind,
                                 uint32_t index, uint32_t offset, const Value &value, uint32_t mask,
                                 uint32_t swizzle, bool literalIndex, bool literalOffset,
                                 uint32_t returns = 0) {
        checkCancellation(cancelled);
        if (failed)
            throw std::runtime_error("Recreate the native memory assist after a failed operation");
        const auto typed = typedBufferFormat(view.format);
        if (buffer.bytes.empty() || buffer.bytes.size() > 256ull * 1024 * 1024 ||
            (!typed && buffer.bytes.size() % 4) || !view.resource || !view.size ||
            view.offset > buffer.bytes.size() || view.size > buffer.bytes.size() - view.offset ||
            (view.stride && (buffer.bytes.size() % view.stride || view.offset % view.stride)))
            throw std::runtime_error("Invalid native memory assist storage or view");
        try {
            if (typed) {
                UINT support = 0;
                check(device->CheckFormatSupport(DXGI_FORMAT(view.format), &support),
                      "Check typed buffer format support");
                const UINT required = D3D11_FORMAT_SUPPORT_BUFFER |
                                      (kind == 7 ? D3D11_FORMAT_SUPPORT_SHADER_LOAD
                                                 : D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
                if ((support & required) != required)
                    throw std::runtime_error("Selected device does not support this typed buffer view");
                if (kind == 30) {
                    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 info{DXGI_FORMAT(view.format), 0};
                    const auto hr =
                        device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &info, sizeof info);
                    const auto flag =
                        store ? D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE : D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
                    if (FAILED(hr) || !(info.OutFormatSupport2 & flag))
                        throw std::runtime_error("Selected device does not support this typed UAV operation");
                }
            }
            const auto words = typed ? typedKernel(store, kind, mask, swizzle, index, literalIndex, returns)
                                     : kernel(store, view.stride, kind, mask, swizzle, index, offset,
                                              literalIndex, literalOffset);
            auto found = shaders.find(words);
            if (found == shaders.end()) {
                if (shaders.size() >= 256)
                    shaders.clear();
                std::vector<uint8_t> code(words.size() * 4);
                std::memcpy(code.data(), words.data(), code.size());
                const auto binary = makeDxbc({{0x58454853, code}});
                Com<ID3D11ComputeShader> shader;
                check(device->CreateComputeShader(binary.data(), binary.size(), nullptr, &shader),
                      "Create native buffer operation shader");
                found = shaders.emplace(words, std::move(shader)).first;
            }
            D3D11_BUFFER_DESC desc{UINT(buffer.bytes.size()),
                                   D3D11_USAGE_DEFAULT,
                                   UINT(kind == 7 ? D3D11_BIND_SHADER_RESOURCE : D3D11_BIND_UNORDERED_ACCESS),
                                   0,
                                   UINT(typed         ? 0
                                        : view.stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                                      : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS),
                                   view.stride};
            D3D11_SUBRESOURCE_DATA initial{buffer.bytes.data(), 0, 0};
            Com<ID3D11Buffer> resource, output, staging, constants;
            check(device->CreateBuffer(&desc, &initial, &resource), "Create private native buffer snapshot");
            const UINT unit = typed ? typed->bytes : view.stride ? view.stride : 4;
            Com<ID3D11ShaderResourceView> srv;
            Com<ID3D11UnorderedAccessView> uav, resultView;
            if (kind == 7) {
                D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                v.ViewDimension = typed ? D3D11_SRV_DIMENSION_BUFFER : D3D11_SRV_DIMENSION_BUFFEREX;
                v.Format = typed         ? DXGI_FORMAT(view.format)
                           : view.stride ? DXGI_FORMAT_UNKNOWN
                                         : DXGI_FORMAT_R32_TYPELESS;
                v.BufferEx = {UINT(view.offset / unit), UINT(view.size / unit),
                              typed || view.stride ? 0u : D3D11_BUFFEREX_SRV_FLAG_RAW};
                check(device->CreateShaderResourceView(resource.Get(), &v, &srv),
                      "Create private native buffer SRV");
            } else {
                D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
                v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                v.Format = typed         ? DXGI_FORMAT(view.format)
                           : view.stride ? DXGI_FORMAT_UNKNOWN
                                         : DXGI_FORMAT_R32_TYPELESS;
                v.Buffer = {UINT(view.offset / unit), UINT(view.size / unit),
                            typed || view.stride ? 0u : D3D11_BUFFER_UAV_FLAG_RAW};
                check(device->CreateUnorderedAccessView(resource.Get(), &v, &uav),
                      "Create private native buffer UAV");
            }
            if (!store) {
                desc = {16,
                        D3D11_USAGE_DEFAULT,
                        D3D11_BIND_UNORDERED_ACCESS,
                        0,
                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                        16};
                check(device->CreateBuffer(&desc, nullptr, &output), "Create native buffer load output");
                D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
                v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                v.Buffer.NumElements = 1;
                check(device->CreateUnorderedAccessView(output.Get(), &v, &resultView),
                      "Create native buffer load output view");
            }
            std::array<uint32_t, 8> parameters{
                index, offset, 0, 0, value.bits[0], value.bits[1], value.bits[2], value.bits[3]};
            desc = {32, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            initial.pSysMem = parameters.data();
            check(device->CreateBuffer(&desc, &initial, &constants),
                  "Create native buffer operation parameters");
            const UINT bytes = store ? UINT(buffer.bytes.size()) : 16;
            desc = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            check(device->CreateBuffer(&desc, nullptr, &staging), "Create native buffer operation staging");
            auto *cb = constants.Get();
            auto *s = srv.Get();
            ID3D11UnorderedAccessView *targets[]{uav.Get(), resultView.Get()};
            context->CSSetShader(found->second.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, &cb);
            context->CSSetShaderResources(0, 1, &s);
            context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
            context->Dispatch(1, 1, 1);
            context->ClearState();
            context->CopyResource(staging.Get(), store ? resource.Get() : output.Get());
            context->Flush();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            std::vector<uint8_t> result(bytes);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            for (;;) {
                checkCancellation(cancelled);
                const auto hr =
                    context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == S_OK)
                    break;
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                    check(hr, "Read native buffer operation result");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Native buffer operation timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::memcpy(result.data(), mapped.pData, bytes);
            context->Unmap(staging.Get(), 0);
            if (debug) {
                Com<ID3D11InfoQueue> queue;
                check(device.As(&queue), "Read native memory validation queue");
                for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                    SIZE_T size = 0;
                    check(queue->GetMessage(i, nullptr, &size), "Read native memory validation message size");
                    std::vector<uint8_t> data(size);
                    auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                    check(queue->GetMessage(i, message, &size), "Read native memory validation message");
                    if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                        throw std::runtime_error(message->pDescription);
                }
                queue->ClearStoredMessages();
            }
            ++count;
            return result;
        } catch (...) {
            context->ClearState();
            failed = true;
            throw;
        }
    }
};
GpuOperations::GpuOperations(ID3D11Device *reference, bool warp, CancelCheck cancelled, uint32_t timeoutMs)
    : state_(std::make_unique<State>()) {
    if (!reference || !timeoutMs || timeoutMs > 60000)
        throw std::invalid_argument("Invalid native memory assist configuration");
    auto &s = *state_;
    s.warp = warp;
    s.cancelled = std::move(cancelled);
    s.timeoutMs = timeoutMs;
    checkCancellation(s.cancelled);
    Com<IDXGIAdapter> original;
    const auto expected = adapter(reference, original);
    const auto level = reference->GetFeatureLevel();
    if (level < D3D_FEATURE_LEVEL_11_0)
        throw std::runtime_error("Native memory assistance requires feature level 11.0");
    s.debug = (reference->GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG) != 0;
    check(D3D11CreateDevice(warp ? nullptr : original.Get(),
                            warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            s.debug ? D3D11_CREATE_DEVICE_DEBUG : 0, &level, 1, D3D11_SDK_VERSION, &s.device,
                            nullptr, &s.context),
          "Create isolated native memory device");
    Com<IDXGIAdapter> actual;
    s.description = adapter(s.device.Get(), actual);
    if (s.description.AdapterLuid.HighPart != expected.AdapterLuid.HighPart ||
        s.description.AdapterLuid.LowPart != expected.AdapterLuid.LowPart)
        throw std::runtime_error("Native memory assist adapter differs from the reference adapter");
    check(actual->CheckInterfaceSupport(__uuidof(IDXGIDevice), &s.driver),
          "Read native memory assist driver identity");
}
GpuOperations::~GpuOperations() = default;
nlohmann::json GpuOperations::identity() const {
    const auto &s = *state_;
    return {{"luid_low", s.description.AdapterLuid.LowPart},
            {"luid_high", s.description.AdapterLuid.HighPart},
            {"vendor_id", s.description.VendorId},
            {"device_id", s.description.DeviceId},
            {"driver_version", s.driver.QuadPart},
            {"feature_level", uint32_t(s.device->GetFeatureLevel())},
            {"warp", s.warp},
            {"backend", "dxbc-gpu/9"}};
}
uint64_t GpuOperations::operations() const { return state_->count; }
std::optional<uint64_t> GpuOperations::textureUavFootprint(const TextureInput &input,
                                                           const TextureInputView &view, uint32_t dimension,
                                                           uint32_t returns, const Value &address) {
    auto &s = *state_;
    checkCancellation(s.cancelled);
    if (s.failed)
        throw std::runtime_error("Recreate the native GPU assist after a failed operation");
    Words key = input.resource.desc;
    key.insert(key.end(), view.nativeDescriptor.begin(), view.nativeDescriptor.end());
    key.insert(key.end(), address.bits.begin(), address.bits.end());
    key.insert(key.end(), {dimension, returns, std::bit_cast<uint32_t>(input.minLod)});
    if (auto found = s.textureFootprints.find(key); found != s.textureFootprints.end())
        return found->second;
    const auto format = typedBufferFormat(view.format);
    if (!format || input.storage.bytes.size() > 256ull * 1024 * 1024)
        throw std::runtime_error("Invalid texture UAV footprint request");
    auto marker = input;
    std::fill(marker.storage.bytes.begin(), marker.storage.bytes.end(), 0);
    const uint32_t one = format->returnType == 5 ? 0x3f800000u : 1u;
    const auto after = textureUav(marker, view, dimension, returns, address,
                                  Value::known({one, one, one, one}), true, {0, 1, 2, 3}, 15)
                           .second;
    std::optional<uint64_t> selected;
    for (size_t i = 0; i < after.size(); ++i)
        if (after[i]) {
            const auto texel = uint64_t(i / format->bytes) * format->bytes;
            if (selected && *selected != texel)
                throw std::runtime_error("Native texture UAV footprint covers multiple texels");
            selected = texel;
        }
    if (s.textureFootprints.size() >= 256)
        s.textureFootprints.clear();
    s.textureFootprints.emplace(std::move(key), selected);
    return selected;
}
std::pair<Value, std::vector<uint8_t>>
GpuOperations::textureUav(const TextureInput &input, const TextureInputView &view, uint32_t dimension,
                          uint32_t returns, const Value &address, const Value &value, bool store,
                          const std::array<uint32_t, 4> &swizzle, uint32_t mask) {
    auto &s = *state_;
    checkCancellation(s.cancelled);
    if (s.failed)
        throw std::runtime_error("Recreate the native GPU assist after a failed operation");
    if (input.storage.bytes.empty() || input.storage.bytes.size() > 256ull * 1024 * 1024 ||
        view.nativeDescriptor.size() != 5 ||
        (dimension != 2 && dimension != 3 && dimension != 5 && dimension != 7 && dimension != 8))
        throw std::runtime_error("Invalid native texture UAV operation");
    try {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{DXGI_FORMAT(view.format), 0};
        check(s.device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof support),
              "Query native texture UAV support");
        const auto required =
            store ? D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE : D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
        if (!(support.OutFormatSupport2 & required))
            throw std::runtime_error("Selected adapter does not support this typed texture UAV operation");
        const auto layout = textureInitialSubresources(input.resource, input.storage.bytes);
        std::vector<D3D11_SUBRESOURCE_DATA> initial;
        for (const auto &sub : layout)
            initial.push_back({input.storage.bytes.data() + sub.offset, sub.rowPitch, UINT(sub.slicePitch)});
        Words resourceKey{input.resource.type, uint32_t(view.resource.value_or(0)),
                          uint32_t(view.resource.value_or(0) >> 32), uint32_t(input.resource.desc.size())};
        resourceKey.insert(resourceKey.end(), input.resource.desc.begin(), input.resource.desc.end());
        resourceKey.insert(resourceKey.end(), view.nativeDescriptor.begin(), view.nativeDescriptor.end());
        resourceKey.push_back(std::bit_cast<uint32_t>(input.minLod));
        auto cached = s.textureUavs.find(resourceKey);
        if (cached == s.textureUavs.end()) {
            if (s.textureUavs.size() >= 64 ||
                s.textureUavBytes + input.storage.bytes.size() > 256ull * 1024 * 1024) {
                s.textureUavs.clear();
                s.textureUavBytes = 0;
            }
            cached = s.textureUavs.emplace(std::move(resourceKey), State::TextureUavOperation{}).first;
            s.textureUavBytes += input.storage.bytes.size();
        }
        auto &operation = cached->second;
        auto &texture = operation.texture;
        auto &textureStaging = operation.staging;
        auto create = [&]<class T, class D>(auto method) {
            if (input.resource.desc.size() * 4 != sizeof(D))
                throw std::runtime_error("Native texture UAV descriptor size mismatch");
            D desc{};
            std::memcpy(&desc, input.resource.desc.data(), sizeof desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags &= D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
            Com<T> object;
            check((s.device.Get()->*method)(&desc, initial.data(), &object),
                  "Create private texture UAV storage");
            texture = object;
            {
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;
                object.Reset();
                check((s.device.Get()->*method)(&desc, nullptr, &object), "Create texture UAV staging");
                textureStaging = object;
            }
        };
        if (!texture) {
            if (input.resource.type == 0x84)
                create.operator()<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>(&ID3D11Device::CreateTexture1D);
            else if (input.resource.type == 0x85)
                create.operator()<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>(&ID3D11Device::CreateTexture2D);
            else if (input.resource.type == 0x86)
                create.operator()<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>(&ID3D11Device::CreateTexture3D);
            else
                throw std::runtime_error("Unsupported native texture UAV storage");
            if (hasResourceLodClamp(input.resource))
                s.context->SetResourceMinLOD(texture.Get(), input.minLod);
            D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
            std::memcpy(&vd, view.nativeDescriptor.data(), sizeof vd);
            check(s.device->CreateUnorderedAccessView(texture.Get(), &vd, &operation.view),
                  "Create private texture UAV");
            operation.shadow = input.storage.bytes;
        } else if (operation.shadow != input.storage.bytes) {
            // CPU rollback and writes through other views invalidate the private GPU copy.
            for (const auto &sub : layout)
                s.context->UpdateSubresource(texture.Get(), sub.index, nullptr,
                                             input.storage.bytes.data() + sub.offset, sub.rowPitch,
                                             UINT(sub.slicePitch));
            operation.shadow = input.storage.bytes;
        }
        std::vector<Words> rows{row(89, {{0x208e46, 0, 2}}), row(104, {{1}}), row(155, {{1, 1, 1}}),
                                row(156 | (dimension << 11), {dest(30, 0), {returns}})};
        if (store)
            rows.push_back(row(164, {dest(30, 0), {0x208e46, 0, 0}, {0x208e46, 0, 1}}));
        else {
            uint32_t selection = 0;
            for (unsigned c = 0; c < 4; ++c)
                selection |= swizzle[c] << (c * 2);
            rows.push_back(row(158, {dest(30, 1), {16}}));
            rows.push_back(row(54, {dest(0, 0), immediate(0)}));
            rows.push_back(row(163, {dest(0, 0, mask), {0x208e46, 0, 0}, source(30, 0, selection)}));
            rows.push_back(row(168, {dest(30, 1), immediate(0), immediate(0), source(0, 0)}));
        }
        rows.push_back(row(62, {}));
        const auto bytes = writeDxbcProgram({{0x50050, 0}, rows});
        Words key(bytes.size() / 4);
        std::memcpy(key.data(), bytes.data(), bytes.size());
        auto found = s.shaders.find(key);
        if (found == s.shaders.end()) {
            if (s.shaders.size() >= 256)
                s.shaders.clear();
            const auto binary = makeDxbc({{0x58454853, bytes}});
            Com<ID3D11ComputeShader> shader;
            check(s.device->CreateComputeShader(binary.data(), binary.size(), nullptr, &shader),
                  "Create native texture UAV shader");
            found = s.shaders.emplace(std::move(key), std::move(shader)).first;
        }
        const std::array<uint32_t, 8> parameters{address.bits[0], address.bits[1], address.bits[2],
                                                 address.bits[3], value.bits[0],   value.bits[1],
                                                 value.bits[2],   value.bits[3]};
        D3D11_BUFFER_DESC bd{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (!s.textureUavConstants)
            check(s.device->CreateBuffer(&bd, nullptr, &s.textureUavConstants),
                  "Create texture UAV constants");
        s.context->UpdateSubresource(s.textureUavConstants.Get(), 0, nullptr, parameters.data(), 0, 0);
        if (!store && !s.textureUavOutput) {
            bd = {16,
                  D3D11_USAGE_DEFAULT,
                  D3D11_BIND_UNORDERED_ACCESS,
                  0,
                  D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                  16};
            check(s.device->CreateBuffer(&bd, nullptr, &s.textureUavOutput),
                  "Create texture UAV load output");
            check(s.device->CreateUnorderedAccessView(s.textureUavOutput.Get(), nullptr,
                                                      &s.textureUavOutputView),
                  "Create texture UAV load output view");
            bd = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            check(s.device->CreateBuffer(&bd, nullptr, &s.textureUavStaging),
                  "Create texture UAV load staging");
        }
        auto *cb = s.textureUavConstants.Get();
        ID3D11UnorderedAccessView *targets[]{operation.view.Get(),
                                             store ? nullptr : s.textureUavOutputView.Get()};
        s.context->CSSetShader(found->second.Get(), nullptr, 0);
        s.context->CSSetConstantBuffers(0, 1, &cb);
        s.context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
        s.context->Dispatch(1, 1, 1);
        s.context->ClearState();
        ID3D11Resource *staging = store ? textureStaging.Get() : s.textureUavStaging.Get();
        s.context->CopyResource(staging, store ? texture.Get() : s.textureUavOutput.Get());
        s.context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.timeoutMs);
        const auto yieldUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(500);
        auto map = [&](uint32_t subresource) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            for (;;) {
                checkCancellation(s.cancelled);
                const auto hr =
                    s.context->Map(staging, subresource, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == S_OK)
                    return mapped;
                if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                    check(hr, "Read native texture UAV result");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Native texture UAV operation timed out");
                // Avoid a coarse scheduler tick for short copies; keep busy polling bounded.
                if (std::chrono::steady_clock::now() < yieldUntil)
                    std::this_thread::yield();
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        std::pair<Value, std::vector<uint8_t>> result;
        if (store) {
            result.second.resize(input.storage.bytes.size());
            for (const auto &sub : layout) {
                const auto mapped = map(sub.index);
                if (mapped.RowPitch < sub.rowPitch ||
                    (sub.depth > 1 && mapped.DepthPitch < uint64_t(mapped.RowPitch) * sub.height)) {
                    s.context->Unmap(staging, sub.index);
                    throw std::runtime_error("Native texture UAV readback pitch is too small");
                }
                for (uint32_t z = 0; z < sub.depth; ++z)
                    for (uint32_t y = 0; y < sub.height; ++y)
                        std::memcpy(result.second.data() + sub.offset + uint64_t(z) * sub.slicePitch +
                                        uint64_t(y) * sub.rowPitch,
                                    static_cast<const uint8_t *>(mapped.pData) +
                                        uint64_t(z) * mapped.DepthPitch + uint64_t(y) * mapped.RowPitch,
                                    sub.rowPitch);
                s.context->Unmap(staging, sub.index);
            }
        } else {
            const auto mapped = map(0);
            std::memcpy(result.first.bits.data(), mapped.pData, 16);
            s.context->Unmap(staging, 0);
        }
        if (s.debug) {
            Com<ID3D11InfoQueue> queue;
            check(s.device.As(&queue), "Read texture UAV validation queue");
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                check(queue->GetMessage(i, nullptr, &size), "Read texture UAV validation size");
                std::vector<uint8_t> data(size);
                auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                check(queue->GetMessage(i, message, &size), "Read texture UAV validation message");
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
            queue->ClearStoredMessages();
        }
        if (store)
            operation.shadow = result.second;
        ++s.count;
        return result;
    } catch (...) {
        s.context->ClearState();
        s.failed = true;
        throw;
    }
}
Value GpuOperations::textureDimensions(const TextureMetadata &input, const TextureInputView &view,
                                       uint32_t kind, uint32_t dimension, uint32_t mip, uint32_t mode) {
    auto &s = *state_;
    checkCancellation(s.cancelled);
    if (s.failed)
        throw std::runtime_error("Recreate the native GPU assist after a failed operation");
    const bool uav = kind == 30;
    if ((!uav && kind != 7) || dimension < 2 || dimension > 10 || mode > 2 ||
        view.nativeDescriptor.size() != (uav ? 5u : 6u))
        throw std::runtime_error("Invalid native resource query");
    Words key = input.resource.desc;
    key.insert(key.end(), view.nativeDescriptor.begin(), view.nativeDescriptor.end());
    key.insert(key.end(), {kind, dimension, mip, mode, std::bit_cast<uint32_t>(input.minLod)});
    if (auto found = s.textureDimensionResults.find(key); found != s.textureDimensionResults.end())
        return found->second;
    const auto layout = textureSubresources(input.resource);
    const auto info = textureInfo(input.resource);
    if (layout.empty() || layout.back().offset + layout.back().size > (256ull * 1024 * 1024) / info.samples)
        throw std::runtime_error("Native resource query exceeds the private texture budget");
    try {
        Com<ID3D11Resource> texture;
        auto create = [&]<class T, class D>(auto method) {
            if (input.resource.desc.size() * 4 != sizeof(D))
                throw std::runtime_error("Native resource query descriptor size mismatch");
            D desc{};
            std::memcpy(&desc, input.resource.desc.data(), sizeof desc);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = uav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
            Com<T> object;
            check((s.device.Get()->*method)(&desc, nullptr, &object),
                  "Create private resource query texture");
            texture = object;
        };
        if (input.resource.type == 0x84)
            create.operator()<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>(&ID3D11Device::CreateTexture1D);
        else if (input.resource.type == 0x85)
            create.operator()<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>(&ID3D11Device::CreateTexture2D);
        else if (input.resource.type == 0x86)
            create.operator()<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>(&ID3D11Device::CreateTexture3D);
        else
            throw std::runtime_error("Unsupported native resource query type");
        if (hasResourceLodClamp(input.resource))
            s.context->SetResourceMinLOD(texture.Get(), input.minLod);
        Com<ID3D11UnorderedAccessView> viewObject, target;
        Com<ID3D11ShaderResourceView> srv;
        if (uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
            std::memcpy(&vd, view.nativeDescriptor.data(), sizeof vd);
            check(s.device->CreateUnorderedAccessView(texture.Get(), &vd, &viewObject),
                  "Create private resource query UAV");
        } else {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            std::memcpy(&vd, view.nativeDescriptor.data(), sizeof vd);
            check(s.device->CreateShaderResourceView(texture.Get(), &vd, &srv),
                  "Create private resource query SRV");
        }
        const auto bytes = writeDxbcProgram(
            {{0x50050, 0},
             {row(89, {{0x208e46, 0, 1}}), row(104, {{1}}), row(155, {{1, 1, 1}}),
              row((uav ? 156u : 88u) | (dimension << 11) |
                      ((dimension == 4 || dimension == 9) ? info.samples << 16 : 0u),
                  {dest(kind, 0), {0x4444}}),
              row(158, {dest(30, 1), {16}}),
              row(61 | (mode << 11), {dest(0, 0), {0x20800a, 0, 0}, source(kind, 0)}),
              row(168, {dest(30, 1), immediate(0), immediate(0), source(0, 0)}), row(62, {})}});
        const auto binary = makeDxbc({{0x58454853, bytes}});
        Com<ID3D11ComputeShader> shader;
        check(s.device->CreateComputeShader(binary.data(), binary.size(), nullptr, &shader),
              "Create native resource query shader");
        D3D11_BUFFER_DESC bd{
            16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            16};
        Com<ID3D11Buffer> output, staging;
        check(s.device->CreateBuffer(&bd, nullptr, &output), "Create resource query output");
        check(s.device->CreateUnorderedAccessView(output.Get(), nullptr, &target),
              "Create resource query output view");
        bd = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        check(s.device->CreateBuffer(&bd, nullptr, &staging), "Create resource query staging");
        const std::array<uint32_t, 4> parameters{mip, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA initial{parameters.data(), 0, 0};
        bd = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        Com<ID3D11Buffer> constants;
        check(s.device->CreateBuffer(&bd, &initial, &constants), "Create resource query constants");
        auto *cb = constants.Get();
        auto *sourceView = srv.Get();
        ID3D11UnorderedAccessView *targets[]{viewObject.Get(), target.Get()};
        s.context->CSSetShader(shader.Get(), nullptr, 0);
        s.context->CSSetConstantBuffers(0, 1, &cb);
        s.context->CSSetShaderResources(0, 1, &sourceView);
        s.context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
        s.context->Dispatch(1, 1, 1);
        s.context->ClearState();
        s.context->CopyResource(staging.Get(), output.Get());
        s.context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.timeoutMs);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        for (;;) {
            checkCancellation(s.cancelled);
            const auto hr =
                s.context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == S_OK)
                break;
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
                check(hr, "Read native resource query result");
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Native resource query timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Value result = Value::known({});
        std::memcpy(result.bits.data(), mapped.pData, 16);
        s.context->Unmap(staging.Get(), 0);
        if (s.debug) {
            Com<ID3D11InfoQueue> queue;
            check(s.device.As(&queue), "Read resource query validation queue");
            for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T size = 0;
                check(queue->GetMessage(i, nullptr, &size), "Read resource query validation size");
                std::vector<uint8_t> data(size);
                auto *message = reinterpret_cast<D3D11_MESSAGE *>(data.data());
                check(queue->GetMessage(i, message, &size), "Read resource query validation message");
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
            queue->ClearStoredMessages();
        }
        if (s.textureDimensionResults.size() >= 256)
            s.textureDimensionResults.clear();
        s.textureDimensionResults.emplace(std::move(key), result);
        ++s.count;
        return result;
    } catch (...) {
        s.context->ClearState();
        s.failed = true;
        throw;
    }
}
Value GpuOperations::loadTexture(uint32_t stage, const TextureInput &input, const TextureInputView &view,
                                 uint32_t dimension, uint32_t returns, const Value &address,
                                 const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                                 const std::array<int32_t, 3> &offset) {
    uint32_t selection = 0;
    for (unsigned c = 0; c < 4; ++c)
        selection |= swizzle[c] << (2 * c);
    return state_->texture(stage, input, view, dimension, returns, address, selection, mask, offset);
}
Value GpuOperations::loadTyped(uint32_t stage, const Buffer &buffer, const BufferView &view, uint32_t kind,
                               uint32_t index, uint32_t returns, const std::array<uint32_t, 4> &swizzle,
                               uint32_t mask, bool literal) {
    uint32_t selection = 0;
    for (unsigned c = 0; c < 4; ++c)
        selection |= swizzle[c] << (c * 2);
    if ((stage == 0 || stage == 1) && kind == 7)
        return state_->typedGraphics(stage, buffer, view, index, returns, selection, mask, literal);
    if (stage != 5)
        throw std::runtime_error("Unsupported native typed buffer shader stage");
    const auto bytes =
        state_->execute(buffer, view, false, kind, index, 0, {}, mask, selection, literal, false, returns);
    Value result;
    std::memcpy(result.bits.data(), bytes.data(), 16);
    return result;
}
std::vector<uint8_t> GpuOperations::storeTyped(const Buffer &buffer, const BufferView &view, uint32_t index,
                                               uint32_t returns, const Value &value, bool literal) {
    return state_->execute(buffer, view, true, 30, index, 0, value, 15, 0xe4, literal, false, returns);
}
std::vector<Value> GpuOperations::instruction(uint32_t stage, const Instruction &instruction,
                                              const std::vector<Register> &targets,
                                              const std::vector<uint32_t> &masks,
                                              const std::vector<Value> &sources,
                                              const std::vector<Value> &before) {
    if (stage != 1)
        throw std::runtime_error("Native instruction assistance currently requires a vertex shader");
    const auto operands = checkpoint::instructionOperands(instruction.words);
    if (targets.empty() || targets.size() > 2 || masks.size() != targets.size() ||
        before.size() != targets.size() || operands.ranges.size() != targets.size() + sources.size())
        throw std::runtime_error("Invalid native instruction assist operands");
    std::map<Register, uint32_t> indices;
    uint32_t temps = 0, outputs = 0;
    for (const auto &target : targets) {
        if ((target.kind != 0 && target.kind != 2) || target.indices.size() != 1)
            throw std::runtime_error("Unsupported native instruction assist destination");
        if (!indices.contains(target))
            indices[target] = target.kind == 0 ? temps++ : outputs++;
    }
    if (outputs > 1)
        throw std::runtime_error("Native instruction assistance requires at most one output register");
    std::vector<Value> constants = sources;
    constants.insert(constants.end(), before.begin(), before.end());
    auto operation = operands.prefix;
    for (size_t i = 0; i < operands.ranges.size(); ++i) {
        Words arg;
        if (i < targets.size())
            arg = dest(targets[i].kind, indices.at(targets[i]), masks[i]);
        else {
            const auto [begin, end] = operands.ranges[i];
            if (((instruction.words[begin] >> 12) & 255) == 4)
                arg.assign(instruction.words.begin() + begin, instruction.words.begin() + end);
            else
                arg = {0x208e46, 0, uint32_t(i - targets.size())};
        }
        operation.insert(operation.end(), arg.begin(), arg.end());
    }
    operation[0] = (operation[0] & 0x80ffffff) | (uint32_t(operation.size()) << 24);
    std::vector<Value> result;
    for (size_t selected = 0; selected < targets.size(); ++selected) {
        if (selected && targets[selected] == targets[0]) {
            result.push_back(result.front());
            continue;
        }
        std::vector<Words> rows{row(89, {{0x208e46, 0, uint32_t(constants.size())}}),
                                row(104, {{std::max(temps, 1u)}}), row(101, {dest(2, 0)})};
        // Initialize only components the operation does not write. Literal
        // operands remain literal to preserve the driver's instruction lowering.
        for (size_t i = 0; i < targets.size(); ++i) {
            if (i && targets[i] == targets[0])
                continue;
            uint32_t written = masks[i];
            for (size_t j = i + 1; j < targets.size(); ++j)
                if (targets[i] == targets[j])
                    written |= masks[j];
            if (written != 15)
                rows.push_back(row(54, {dest(targets[i].kind, indices.at(targets[i]), 15 ^ written),
                                        {0x208e46, 0, uint32_t(sources.size() + i)}}));
        }
        rows.push_back(operation);
        if (targets[selected].kind == 0)
            rows.push_back(row(54, {dest(2, 0), source(0, indices.at(targets[selected]))}));
        rows.push_back(row(62, {}));
        const auto bytes = writeDxbcProgram({{0x10050, 0}, rows});
        Words words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), bytes.size());
        result.push_back(state_->vertex(words, constants));
    }
    return result;
}
Value GpuOperations::load(const Buffer &buffer, const BufferView &view, uint32_t kind, uint32_t index,
                          uint32_t offset, const std::array<uint32_t, 4> &swizzle, uint32_t mask,
                          bool literalIndex, bool literalOffset) {
    uint32_t selection = 0;
    for (uint32_t c = 0; c < 4; ++c)
        selection |= swizzle[c] << (2 * c);
    const auto bytes = state_->execute(buffer, view, false, kind, index, offset, {}, mask, selection,
                                       literalIndex, literalOffset);
    Value result;
    std::memcpy(result.bits.data(), bytes.data(), 16);
    if (std::all_of(buffer.valid.begin(), buffer.valid.end(), [](uint8_t v) { return v != 0; }))
        result.valid = uint8_t(mask);
    return result;
}
std::vector<uint8_t> GpuOperations::store(const Buffer &buffer, const BufferView &view, uint32_t index,
                                          uint32_t offset, const Value &value, uint32_t mask,
                                          bool literalIndex, bool literalOffset) {
    return state_->execute(buffer, view, true, 30, index, offset, value, mask, 0xe4, literalIndex,
                           literalOffset);
}
} // namespace flora::dxbc_exec
