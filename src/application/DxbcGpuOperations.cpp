#include "DxbcGpuOperations.h"
#include "DxbcCheckpointModel.h"
#include "HlslCompilation.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <algorithm>
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
} // namespace
struct GpuOperations::State {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    std::map<Words, Com<ID3D11ComputeShader>> shaders;
    struct VertexOperation {
        Com<ID3D11VertexShader> vertex;
        Com<ID3D11GeometryShader> stream;
    };
    std::map<Words, VertexOperation> vertexShaders;
    DXGI_ADAPTER_DESC description{};
    LARGE_INTEGER driver{};
    CancelCheck cancelled;
    uint32_t timeoutMs;
    uint64_t count = 0;
    bool failed = false, warp = false, debug = false;
    Value vertex(const Words &words, const std::vector<Value> &constants) {
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
                parts.emplace_back(0x58454853, std::move(bytes));
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
    std::vector<uint8_t> execute(const Buffer &buffer, const BufferView &view, bool store, uint32_t kind,
                                 uint32_t index, uint32_t offset, const Value &value, uint32_t mask,
                                 uint32_t swizzle, bool literalIndex, bool literalOffset) {
        checkCancellation(cancelled);
        if (failed)
            throw std::runtime_error("Recreate the native memory assist after a failed operation");
        if (buffer.bytes.empty() || buffer.bytes.size() > 256ull * 1024 * 1024 || buffer.bytes.size() % 4 ||
            !view.resource || !view.size || view.offset > buffer.bytes.size() ||
            view.size > buffer.bytes.size() - view.offset ||
            (view.stride && (buffer.bytes.size() % view.stride || view.offset % view.stride)))
            throw std::runtime_error("Invalid native memory assist storage or view");
        try {
            const auto words =
                kernel(store, view.stride, kind, mask, swizzle, index, offset, literalIndex, literalOffset);
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
                                   UINT(view.stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED
                                                    : D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS),
                                   view.stride};
            D3D11_SUBRESOURCE_DATA initial{buffer.bytes.data(), 0, 0};
            Com<ID3D11Buffer> resource, output, staging, constants;
            check(device->CreateBuffer(&desc, &initial, &resource), "Create private native buffer snapshot");
            const UINT unit = view.stride ? view.stride : 4;
            Com<ID3D11ShaderResourceView> srv;
            Com<ID3D11UnorderedAccessView> uav, resultView;
            if (kind == 7) {
                D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
                v.Format = view.stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                v.BufferEx = {UINT(view.offset / unit), UINT(view.size / unit),
                              view.stride ? 0u : D3D11_BUFFEREX_SRV_FLAG_RAW};
                check(device->CreateShaderResourceView(resource.Get(), &v, &srv),
                      "Create private native buffer SRV");
            } else {
                D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
                v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                v.Format = view.stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                v.Buffer = {UINT(view.offset / unit), UINT(view.size / unit),
                            view.stride ? 0u : D3D11_BUFFER_UAV_FLAG_RAW};
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
            {"backend", "dxbc-gpu/2"}};
}
uint64_t GpuOperations::operations() const { return state_->count; }
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
