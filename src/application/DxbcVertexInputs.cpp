#include "DxbcVertexInputs.h"
#include "DxbcGpuCompletion.h"
#include "core/Dxbc.h"
#include "replay/Replay.h"
#include <algorithm>
#include <bit>
#include <d3d11sdklayers.h>
#include <set>

namespace flora::dxbc_exec {
namespace {
constexpr uint32_t ISGN = 0x4e475349, ISG1 = 0x31475349, OSGN = 0x4e47534f, SHDR = 0x52444853,
                   SHEX = 0x58454853;
uint32_t word(Bytes b, size_t offset) {
    if (offset > b.size())
        throw std::runtime_error("Vertex input signature bounds");
    return Reader(b.subspan(offset)).read<uint32_t>();
}
void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Vertex input signature write bounds");
    std::memcpy(bytes.data() + offset, &value, 4);
}
std::vector<uint32_t> dest(uint32_t kind, uint32_t reg, uint32_t mask) {
    return {0x100002 | (kind << 12) | (mask << 4), reg};
}
std::vector<uint32_t> instruction(uint32_t op, std::vector<uint32_t> args) {
    args.insert(args.begin(), op | (uint32_t(args.size() + 1) << 24));
    return args;
}
DXGI_ADAPTER_DESC describe(ID3D11Device *device, Com<IDXGIAdapter> &adapter) {
    Com<IDXGIDevice> dxgi;
    check(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "Query vertex input device");
    check(dxgi->GetAdapter(&adapter), "Query vertex input adapter");
    DXGI_ADAPTER_DESC desc{};
    check(adapter->GetDesc(&desc), "Describe vertex input adapter");
    return desc;
}
} // namespace
VertexInputShader vertexInputShader(Bytes original, std::optional<std::array<uint32_t, 2>> systemIds) {
    Bytes input, code;
    uint32_t inputTag = 0, codeTag = 0;
    std::set<uint32_t> tags;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate vertex input DXBC chunk");
        if (tag == ISGN || tag == ISG1) {
            if (inputTag)
                throw std::runtime_error("Conflicting vertex input signatures");
            input = bytes;
            inputTag = tag;
        }
        if (tag == SHDR || tag == SHEX) {
            if (codeTag)
                throw std::runtime_error("Conflicting vertex input programs");
            code = bytes;
            codeTag = tag;
        }
    }
    if (!inputTag || !codeTag)
        throw std::runtime_error("Vertex input capture requires an input signature and executable");
    const auto originalProgram = readDxbcProgram(code);
    const auto version = originalProgram.header[0];
    if (version != 0x10040 && version != 0x10041 && version != 0x10050)
        throw std::runtime_error("Vertex input capture requires an SM4/SM5 vertex shader");
    std::array<uint32_t, 32> declared{}, signature{};
    DxbcProgram program{{version, 0}, {}};
    for (const auto &row : originalProgram.instructions) {
        const auto op = row[0] & 2047;
        if (op != 95 && op != 96 && op != 97)
            continue;
        if (row.size() < 3 || (row[0] & 0x80000000u) || (row[1] & 0x7fffff0fu) != 0x101002)
            throw std::runtime_error("Unsupported vertex input declaration");
        size_t index = 2;
        // Keep the original minimum-precision operand extension. No source
        // modifiers or addressing extensions are valid on an input declaration.
        if (row[1] & 0x80000000u) {
            if (row.size() < 4 || (row[2] & ~0x1c001u) || (row[2] & 63) != 1)
                throw std::runtime_error("Unsupported vertex input operand extension");
            ++index;
        }
        if (row.size() != index + 1 + (op != 95) || row[index] >= 32)
            throw std::runtime_error("Invalid vertex input declaration length/register");
        const auto reg = row[index], mask = (row[1] >> 4) & 15;
        if (!mask || (declared[reg] & mask))
            throw std::runtime_error("Overlapping or empty vertex input declarations");
        declared[reg] |= mask;
        program.instructions.push_back(row);
    }
    const auto count = word(input, 0);
    const size_t rowSize = inputTag == ISG1 ? 32 : 24;
    if (count > 32 || input.size() < 8 || uint64_t(count) * rowSize > input.size() - 8)
        throw std::runtime_error("Vertex input signature row bounds");
    std::vector<uint8_t> inputCopy(input.begin(), input.end());
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> systemComponents;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t base = 8 + i * rowSize + (inputTag == ISG1 ? 4 : 0);
        const auto name = word(input, base), reg = word(input, base + 16), type = word(input, base + 12);
        const auto mask = input[base + 20];
        if (name < 8 + count * rowSize || name >= input.size() ||
            std::find(input.begin() + name, input.end(), 0) == input.end() || reg >= 32 || !mask ||
            mask > 15 || type < 1 || type > 3 || (signature[reg] & mask) ||
            (inputTag == ISG1 && word(input, base - 4) != 0))
            throw std::runtime_error("Invalid vertex input signature row");
        signature[reg] |= mask;
        inputCopy[base + 21] = uint8_t(declared[reg] & mask);
        const auto system = word(input, base + 8);
        if (systemIds && (system == D3D_NAME_VERTEX_ID || system == D3D_NAME_INSTANCE_ID))
            systemComponents[reg].push_back(
                {declared[reg] & mask, (*systemIds)[system == D3D_NAME_INSTANCE_ID ? 1 : 0]});
    }
    VertexInputShader result;
    for (uint32_t reg = 0; reg < 32; ++reg) {
        if (declared[reg] & ~signature[reg])
            throw std::runtime_error("Vertex input declaration exceeds signature");
        if (declared[reg]) {
            result.fields.push_back({reg, declared[reg], result.stride});
            result.stride += 4 * std::popcount(declared[reg]);
        }
    }
    // An input-free shader still needs one SO word to count submitted points.
    const auto outputCount = uint32_t(std::max<size_t>(1, result.fields.size()));
    std::vector<uint8_t> output(8 + outputCount * 24);
    put(output, 0, outputCount);
    put(output, 4, 8);
    for (uint32_t i = 0; i < outputCount; ++i) {
        const auto mask = result.fields.empty() ? 1u : result.fields[i].mask;
        const auto base = 8 + i * 24;
        put(output, base, uint32_t(output.size()));
        put(output, base + 4, i);
        put(output, base + 12, 1); // Unsigned integers preserve every source bit.
        put(output, base + 16, i);
        output[base + 20] = uint8_t(mask);
        output[base + 21] = uint8_t(15 ^ mask);
        constexpr char semantic[] = "FLORA_INPUT";
        output.insert(output.end(), semantic, semantic + sizeof semantic);
        program.instructions.push_back(instruction(101, dest(2, i, mask)));
    }
    while (output.size() % 4)
        output.push_back(0);
    for (uint32_t i = 0; i < outputCount; ++i) {
        const auto mask = result.fields.empty() ? 1u : result.fields[i].mask;
        auto args = dest(2, i, mask);
        if (result.fields.empty())
            args.insert(args.end(), {0x4001, 0});
        else
            args.insert(args.end(), {0x101e46, result.fields[i].reg});
        program.instructions.push_back(instruction(54, std::move(args)));
        if (!result.fields.empty())
            for (const auto &[systemMask, value] : systemComponents[result.fields[i].reg])
                if (systemMask) {
                    auto replacement = dest(2, i, systemMask);
                    replacement.insert(replacement.end(), {0x4001, value});
                    program.instructions.push_back(instruction(54, std::move(replacement)));
                }
    }
    program.instructions.push_back({0x100003e}); // ret
    if (!result.stride)
        result.stride = 4;
    const auto executable = writeDxbcProgram(program);
    result.bytecode = makeDxbc({{inputTag, inputCopy}, {OSGN, output}, {codeTag, executable}});
    return result;
}
std::map<Register, Value> VertexInputShader::decode(Bytes bytes) const {
    if (bytes.size() != stride)
        throw std::runtime_error("Vertex input record length mismatch");
    std::map<Register, Value> result;
    for (const auto &field : fields) {
        Value value;
        value.valid = uint8_t(field.mask);
        auto offset = field.offset;
        for (unsigned c = 0; c < 4; ++c)
            if (field.mask & (1u << c)) {
                value.bits[c] = word(bytes, offset);
                offset += 4;
            }
        result.emplace(Register{1, {field.reg}}, value);
    }
    return result;
}

VertexInputCapture captureVertexInputs(ID3D11Device *reference, bool warp, Bytes original,
                                       const std::vector<VertexInputElement> &originalElements,
                                       const std::map<uint32_t, VertexInputBuffer> &originalBuffers,
                                       const VertexInputDraw &originalDraw, uint64_t maxBytes,
                                       CancelCheck cancelled, uint32_t timeoutMs,
                                       std::optional<VertexInputInvocation> selected) {
    checkCancellation(cancelled);
    auto elements = originalElements;
    auto buffers = originalBuffers;
    auto draw = originalDraw;
    if (!reference || !maxBytes || !timeoutMs || elements.size() > 32 || buffers.size() > 32)
        throw std::runtime_error("Invalid vertex input capture limits/device");
    for (const auto &e : elements)
        if (e.semantic.empty() || e.semantic.find('\0') != std::string::npos || e.slot >= 32 ||
            e.classification > 1 || (!e.classification && e.stepRate))
            throw std::runtime_error("Invalid vertex input layout element");
    std::optional<std::array<uint32_t, 2>> systemIds;
    if (!draw.indexFormat && (!draw.indices.empty() || draw.indexOffset || draw.baseVertex))
        throw std::runtime_error("Non-indexed vertex input request has index state");
    std::vector<uint8_t> selectedIndex;
    if (selected) {
        if (selected->reference >= draw.count || selected->instance >= draw.instances)
            throw std::runtime_error("Selected vertex input is outside the effective draw");
        uint32_t vertexId = selected->reference;
        if (draw.indexFormat) {
            const auto width = draw.indexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u;
            const auto offset =
                uint64_t(draw.indexOffset) + (uint64_t(draw.start) + selected->reference) * width;
            if ((draw.indexFormat != DXGI_FORMAT_R16_UINT && draw.indexFormat != DXGI_FORMAT_R32_UINT) ||
                draw.indexOffset % width || offset > draw.indices.size() ||
                width > draw.indices.size() - offset)
                throw std::runtime_error("Selected vertex input index snapshot bounds/format");
            Reader index(draw.indices.subspan(size_t(offset), width));
            vertexId = width == 2 ? index.read<uint16_t>() : index.read<uint32_t>();
            if (vertexId == (width == 2 ? 0xffffu : UINT32_MAX))
                throw std::runtime_error("Selected vertex input is a cut index");
            selectedIndex.assign(draw.indices.begin() + size_t(offset),
                                 draw.indices.begin() + size_t(offset) + width);
            draw.indices = selectedIndex;
            draw.indexOffset = draw.start = 0;
        } else if (!selected->actualIndex) {
            if (uint64_t(draw.start) + selected->reference > UINT32_MAX)
                throw std::runtime_error("Selected vertex input start overflows UINT32");
            draw.start += selected->reference;
        }
        if (selected->actualIndex) {
            // Use one private R32 index. For UINT32_MAX, base vertex one
            // avoids both a cut index and non-indexed start/count overflow.
            const auto actual = *selected->actualIndex;
            const uint32_t raw = actual == UINT32_MAX ? actual - 1 : actual;
            selectedIndex.resize(4);
            std::memcpy(selectedIndex.data(), &raw, 4);
            draw.indexFormat = DXGI_FORMAT_R32_UINT;
            draw.indexOffset = draw.start = 0;
            draw.indices = selectedIndex;
            draw.baseVertex = actual == UINT32_MAX ? 1 : 0;
        }
        // Rebase each instance slot independently, then keep its original
        // StartInstanceLocation with a zero step rate. A single point has
        // draw-relative IDs of zero, so the copy shader writes the original
        // selected system IDs without running the application's shader body.
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> slots;
        for (const auto &element : elements) {
            const auto description = std::make_pair(element.classification, element.stepRate);
            const auto [it, inserted] = slots.emplace(element.slot, description);
            if (!inserted && it->second != description)
                throw std::runtime_error("Vertex input slot has conflicting instance classifications");
        }
        for (const auto &[slot, description] : slots)
            if (description.first && description.second) {
                const auto it = buffers.find(slot);
                if (it != buffers.end()) {
                    const uint64_t offset =
                        uint64_t(it->second.offset) +
                        uint64_t(it->second.stride) * (selected->instance / description.second);
                    if (offset > UINT32_MAX)
                        throw std::runtime_error("Selected vertex input instance offset overflows UINT32");
                    it->second.offset = uint32_t(offset);
                }
            }
        for (auto &element : elements)
            if (element.classification)
                element.stepRate = 0;
        systemIds = std::array<uint32_t, 2>{vertexId, selected->instance};
        draw.count = draw.instances = 1;
    }
    const auto shader = vertexInputShader(original, systemIds);
    uint64_t references = draw.count;
    if (draw.indexFormat) {
        const auto width = draw.indexFormat == DXGI_FORMAT_R16_UINT ? 2u : 4u;
        const auto offset = uint64_t(draw.indexOffset) + uint64_t(draw.start) * width;
        if ((draw.indexFormat != DXGI_FORMAT_R16_UINT && draw.indexFormat != DXGI_FORMAT_R32_UINT) ||
            draw.indexOffset % width || offset > draw.indices.size() ||
            uint64_t(draw.count) * width > draw.indices.size() - offset)
            throw std::runtime_error("Vertex input index snapshot bounds/format");
        Reader indices(draw.indices.subspan(size_t(offset), size_t(draw.count) * width));
        for (uint32_t i = 0; i < draw.count; ++i) {
            if (!(i % 4096))
                checkCancellation(cancelled);
            const auto index = width == 2 ? indices.read<uint16_t>() : indices.read<uint32_t>();
            if (index == (width == 2 ? 0xffffu : UINT32_MAX))
                --references;
        }
    } else if (!draw.indices.empty() || draw.indexOffset || draw.baseVertex)
        throw std::runtime_error("Non-indexed vertex input request has index state");
    const uint64_t rowCount = references * draw.instances;
    // Bound both the SO/staging buffers and the decoded register maps. The
    // conservative map budget also covers input-free and sparse signatures.
    const uint64_t perRow = shader.stride * 2ull + 64 + 256ull * std::max<size_t>(1, shader.fields.size());
    if (rowCount > maxBytes / perRow || rowCount > UINT32_MAX / shader.stride)
        throw std::runtime_error("Vertex input output exceeds byte budget");
    uint64_t used = rowCount * perRow;
    auto reserve = [&](size_t bytes) {
        checkCancellation(cancelled);
        if (bytes > UINT32_MAX || bytes > maxBytes - used)
            throw std::runtime_error("Vertex input snapshots exceed byte budget");
        used += bytes;
    };
    reserve(draw.indices.size());
    for (const auto &[slot, buffer] : buffers) {
        if (slot >= 32 || buffer.bytes.empty() || buffer.stride > 2048 ||
            originalBuffers.at(slot).offset > buffer.bytes.size())
            throw std::runtime_error("Invalid vertex input buffer snapshot");
        reserve(buffer.bytes.size());
    }
    Com<IDXGIAdapter> expectedAdapter;
    const auto expected = describe(reference, expectedAdapter);
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    const auto level = reference->GetFeatureLevel();
    const auto flags = reference->GetCreationFlags() & D3D11_CREATE_DEVICE_DEBUG;
    check(D3D11CreateDevice(warp ? nullptr : expectedAdapter.Get(),
                            warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &level, 1,
                            D3D11_SDK_VERSION, &device, nullptr, &context),
          "Create private vertex input device");
    Com<IDXGIAdapter> actualAdapter;
    const auto actual = describe(device.Get(), actualAdapter);
    if (actual.AdapterLuid.LowPart != expected.AdapterLuid.LowPart ||
        actual.AdapterLuid.HighPart != expected.AdapterLuid.HighPart)
        throw std::runtime_error("Vertex input device changed the requested adapter");
    VertexInputCapture result;
    result.identity = {{"backend", "dxbc-vertex-inputs/1"},
                       {"adapter_vendor", actual.VendorId},
                       {"adapter_device", actual.DeviceId},
                       {"adapter_luid_low", actual.AdapterLuid.LowPart},
                       {"adapter_luid_high", actual.AdapterLuid.HighPart},
                       {"warp", warp},
                       {"input_references", draw.count},
                       {"records_per_instance", references},
                       {"instances", draw.instances},
                       {"records", rowCount},
                       {"stride", shader.stride},
                       {"budgeted_bytes", used}};
    if (selected) {
        result.identity["backend"] = "dxbc-selected-vertex-inputs/1";
        result.identity["input_reference"] = selected->reference;
        result.identity["instance"] = selected->instance;
        result.identity["original_input_references"] = originalDraw.count;
        result.identity["original_instances"] = originalDraw.instances;
        result.identity["system_ids"] = *systemIds;
        result.identity["system_ids_source"] = "draw-relative-selection";
        result.identity["attributes_source"] = "native-input-assembler";
        result.identity["index_override"] =
            selected->actualIndex ? nlohmann::json(*selected->actualIndex) : nlohmann::json(nullptr);
    }
    if (!rowCount)
        return result;
    checkCancellation(cancelled);
    Com<ID3D11VertexShader> vs;
    check(device->CreateVertexShader(shader.bytecode.data(), shader.bytecode.size(), nullptr, &vs),
          "Create vertex input copy shader");
    std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
    for (const auto &e : elements)
        layout.push_back({e.semantic.c_str(), e.index, DXGI_FORMAT(e.format), e.slot, e.offset,
                          D3D11_INPUT_CLASSIFICATION(e.classification), e.stepRate});
    Com<ID3D11InputLayout> inputLayout;
    if (!layout.empty())
        check(device->CreateInputLayout(layout.data(), UINT(layout.size()), shader.bytecode.data(),
                                        shader.bytecode.size(), &inputLayout),
              "Create private vertex input layout");
    std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
    for (UINT i = 0; i < std::max<size_t>(1, shader.fields.size()); ++i) {
        const auto mask = shader.fields.empty() ? 1u : shader.fields[i].mask;
        // SO component indices are relative to the first signature component.
        const auto first = std::countr_zero(mask);
        for (unsigned c = 0; c < 4; ++c)
            if (mask & (1u << c))
                entries.push_back({0, "FLORA_INPUT", i, BYTE(c - first), 1, 0});
    }
    Com<ID3D11GeometryShader> gs;
    check(device->CreateGeometryShaderWithStreamOutput(shader.bytecode.data(), shader.bytecode.size(),
                                                       entries.data(), UINT(entries.size()), &shader.stride,
                                                       1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs),
          "Create vertex input stream output");
    std::vector<Com<ID3D11Buffer>> privateBuffers;
    auto buffer = [&](Bytes bytes, UINT bind) {
        checkCancellation(cancelled);
        D3D11_BUFFER_DESC desc{UINT(bytes.size()), D3D11_USAGE_IMMUTABLE, bind, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA data{bytes.data(), 0, 0};
        Com<ID3D11Buffer> object;
        check(device->CreateBuffer(&desc, &data, &object), "Create private vertex input buffer");
        privateBuffers.push_back(object);
        return object.Get();
    };
    for (const auto &[slot, b] : buffers) {
        auto object = buffer(b.bytes, D3D11_BIND_VERTEX_BUFFER);
        context->IASetVertexBuffers(slot, 1, &object, &b.stride, &b.offset);
    }
    if (draw.indexFormat)
        context->IASetIndexBuffer(buffer(draw.indices, D3D11_BIND_INDEX_BUFFER),
                                  DXGI_FORMAT(draw.indexFormat), draw.indexOffset);
    D3D11_BUFFER_DESC desc{
        UINT(rowCount * shader.stride), D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
    Com<ID3D11Buffer> output, staging;
    check(device->CreateBuffer(&desc, nullptr, &output), "Create vertex input output buffer");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateBuffer(&desc, nullptr, &staging), "Create vertex input readback buffer");
    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_SO_STATISTICS, 0};
    Com<ID3D11Query> query;
    check(device->CreateQuery(&queryDesc, &query), "Create vertex input count query");
    const UINT zero = 0;
    context->IASetInputLayout(inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->SOSetTargets(1, output.GetAddressOf(), &zero);
    GpuCompletion completion(device.Get(), context.Get(), cancelled, timeoutMs);
    checkCancellation(cancelled);
    context->Begin(query.Get());
    if (draw.indexFormat)
        context->DrawIndexedInstanced(draw.count, draw.instances, draw.start, draw.baseVertex,
                                      draw.startInstance);
    else
        context->DrawInstanced(draw.count, draw.instances, draw.start, draw.startInstance);
    context->End(query.Get());
    context->SOSetTargets(0, nullptr, nullptr);
    context->CopyResource(staging.Get(), output.Get());
    completion.submit();
    D3D11_QUERY_DATA_SO_STATISTICS counts{};
    completion.query(query.Get(), &counts, sizeof counts);
    if (counts.NumPrimitivesWritten != rowCount || counts.PrimitivesStorageNeeded != rowCount)
        throw std::runtime_error("Vertex input record count/overflow mismatch");
    const auto mapped = completion.map(staging.Get());
    struct Unmap {
        ID3D11DeviceContext *context;
        ID3D11Buffer *buffer;
        ~Unmap() { context->Unmap(buffer, 0); }
    } unmap{context.Get(), staging.Get()};
    result.rows.reserve(size_t(rowCount));
    const Bytes bytes(static_cast<const uint8_t *>(mapped.pData), desc.ByteWidth);
    for (size_t i = 0; i < rowCount; ++i) {
        checkCancellation(cancelled);
        result.rows.push_back(shader.decode(bytes.subspan(i * shader.stride, shader.stride)));
    }
    if (flags & D3D11_CREATE_DEVICE_DEBUG) {
        Com<ID3D11InfoQueue> queue;
        check(device.As(&queue), "Read vertex input validation queue");
        for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            checkCancellation(cancelled);
            SIZE_T size = 0;
            check(queue->GetMessage(i, nullptr, &size), "Read vertex input validation size");
            std::vector<uint8_t> storage(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            check(queue->GetMessage(i, message, &size), "Read vertex input validation message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
                throw std::runtime_error(std::string("Vertex input D3D11 validation: ") +
                                         message->pDescription);
        }
    }
    return result;
}
} // namespace flora::dxbc_exec
