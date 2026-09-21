#include "QuadSerial.h"
#include "DrawParameters.h"
#include "DxbcSystemId.h"
#include "core/ClassLinkage.h"
#include "replay/Unpredicated.h"
#include <algorithm>
#include <set>
namespace flora {
using Json = nlohmann::json;
QuadStrip expandQuadStrip(std::span<const uint32_t> values, std::optional<uint32_t> cut, uint32_t factor) {
    if (factor != 2 && factor != 3)
        throw std::runtime_error("Quad strip requires line or triangle primitives");
    QuadStrip result;
    std::vector<uint32_t> tail;
    uint64_t cuts = 0, degenerates = 0;
    bool parity = false;
    for (const auto value : values) {
        if (cut && value == *cut) {
            tail.clear();
            parity = false;
            ++cuts;
            continue;
        }
        tail.push_back(value);
        if (tail.size() < factor)
            continue;
        result.indices.push_back(tail[0]);
        if (factor == 3 && parity) {
            result.indices.push_back(tail[2]);
            result.indices.push_back(tail[1]);
        } else
            result.indices.insert(result.indices.end(), tail.begin() + 1, tail.end());
        degenerates += std::set<uint32_t>(tail.begin(), tail.end()).size() < factor;
        tail.erase(tail.begin());
        parity = !parity;
    }
    result.metadata = {{"restart_markers", cuts},
                       {factor == 3 ? "degenerate_triangles" : "degenerate_lines", degenerates}};
    return result;
}
class QuadSerialState {
    Replay &r;
    State state;
    Json parameters, indirect, strip;
    bool indexed, isStrip;
    uint32_t count, factor, instances, vertexOffset = 0;
    uint64_t stripPrimitives = 0, planned = 0;
    std::map<uint32_t, uint32_t> steps;
    std::vector<uint8_t> raw;
    Com<ID3D11Buffer> stripBuffer;
    std::map<std::pair<uint32_t, uint32_t>, Com<ID3D11VertexShader>> shaders;
    std::vector<Com<ID3D11ClassInstance>> classes;
    Com<ID3D11ClassLinkage> linkage;
    QuadStrip boundStrip() {
        Com<ID3D11Buffer> buffer;
        DXGI_FORMAT format;
        UINT offset;
        r.context_->IAGetIndexBuffer(&buffer, &format, &offset);
        if (!buffer)
            throw std::runtime_error("Indexed strip requires a bound index buffer");
        if (format != DXGI_FORMAT_R16_UINT && format != DXGI_FORMAT_R32_UINT)
            throw std::runtime_error("Indexed strip requires R16_UINT or R32_UINT indices");
        const uint32_t stride = format == DXGI_FORMAT_R16_UINT ? 2 : 4;
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        const uint64_t start =
            uint64_t(offset) + uint64_t(parameters.at("start_index").get<uint32_t>()) * stride;
        const uint64_t size = uint64_t(count) * stride;
        if (offset % stride || start > desc.ByteWidth || size > desc.ByteWidth - start)
            throw std::runtime_error("Indexed strip range is unaligned or outside the bound buffer");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Buffer> staging;
        check(r.device_->CreateBuffer(&desc, nullptr, &staging), "Create Quad index staging");
        {
            Unpredicated guard(r.context_.Get());
            r.context_->CopyResource(staging.Get(), buffer.Get());
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read Quad strip indices");
        std::vector<uint8_t> storage;
        try {
            const auto begin = static_cast<const uint8_t *>(mapped.pData) + size_t(start);
            storage.assign(begin, begin + size_t(size));
        } catch (...) {
            r.context_->Unmap(staging.Get(), 0);
            throw;
        }
        r.context_->Unmap(staging.Get(), 0);
        Reader reader(storage);
        std::vector<uint32_t> indices;
        while (reader.remaining())
            indices.push_back(stride == 2 ? reader.read<uint16_t>() : reader.read<uint32_t>());
        auto result = expandQuadStrip(indices, stride == 2 ? 0xffffu : UINT32_MAX, factor);
        result.metadata.update({{"index_format", uint32_t(format)},
                                {"binding_offset", offset},
                                {"read_offset", start},
                                {"read_bytes", size},
                                {"source_sha256", sha256(storage)},
                                {"source", "bound_gpu_index_buffer"}});
        return result;
    }

  public:
    QuadSerialState(Replay &replay, const Event &event, const State &snapshot) : r(replay), state(snapshot) {
        if (state.topology < 1 || state.topology > 5)
            throw std::runtime_error("Serial Quad requires points, lines or triangles without adjacency");
        if (state.stages[1].shader || state.stages[2].shader || state.stages[3].shader || state.soCount)
            throw std::runtime_error("Serial Quad requires the final-geometry path for HS/DS/GS/SO");
        const auto id = state.stages[0].shader;
        if (!id)
            throw std::runtime_error("Serial Quad requires a vertex shader");
        const auto resolved = resolveDrawParameters(r, event);
        parameters = resolved.parameters;
        indirect = resolved.indirect;
        indexed = parameters.contains("index_count");
        count = parameters.at(indexed ? "index_count" : "vertex_count").get<uint32_t>();
        factor = state.topology == 1 ? 1 : state.topology < 4 ? 2 : 3;
        instances = parameters.value("instance_count", 1u);
        planned = uint64_t(count / factor) * instances;
        if (state.layout) {
            Reader resource(r.frame_.payload(state.layout, 5, 0x82));
            resource.skip(16);
            Reader layout(r.frame_.payload(resource.read<Id>(), 9, 0x84));
            const auto elements = layout.read<uint32_t>();
            if (elements > 32)
                throw std::runtime_error("Input layout exceeds 32 elements");
            std::map<uint32_t, std::pair<uint32_t, uint32_t>> slots;
            for (uint32_t i = 0; i < elements; ++i) {
                layout.skip(16); // semantic data ID, semantic index, format
                const auto slot = layout.read<uint32_t>();
                layout.skip(4); // byte offset does not change the step rate
                const auto classification = layout.read<uint32_t>(), step = layout.read<uint32_t>();
                if (slot >= 32 || classification > 1)
                    throw std::runtime_error("Invalid input slot or classification");
                const auto value = std::pair(classification, step);
                if (slots.contains(slot) && slots.at(slot) != value)
                    throw std::runtime_error("Mixed input classifications/step rates in one slot");
                slots[slot] = value;
            }
            for (const auto &[slot, value] : slots)
                if (value.first)
                    steps[slot] = value.second;
        }
        const auto replacement = r.options_.shaders.find(id);
        const auto bytes = replacement == r.options_.shaders.end()
                               ? r.frame_.shader(r.frame_.resource(id).data)
                               : Bytes(replacement->second);
        raw.assign(bytes.begin(), bytes.end());
        linkage = r.get<ID3D11ClassLinkage>(shaderClassLinkage(r.frame_, id));
        const auto classCount = r.interfaceSlots_.at(id);
        if (classCount > state.stages[0].classCount)
            throw std::runtime_error("Missing Quad diagnostic class instances");
        for (uint32_t i = 0; i < classCount; ++i)
            classes.emplace_back(r.get<ID3D11ClassInstance>(state.stages[0].classes[i]));
        isStrip = state.topology == 3 || state.topology == 5;
        if (isStrip) {
            QuadStrip expanded;
            expanded.metadata = {{"restart_markers", 0},
                                 {"source", "empty_draw"},
                                 {factor == 3 ? "degenerate_triangles" : "degenerate_lines", 0}};
            if (count >= factor && instances) {
                if (indexed)
                    expanded = boundStrip();
                else {
                    const auto start = parameters.at("start_vertex").get<uint32_t>();
                    if (uint64_t(start) + count > 0x100000000ull)
                        throw std::runtime_error(
                            "Nonindexed strip with wrapping vertex addresses is unsupported");
                    std::vector<uint32_t> indices;
                    indices.reserve(count);
                    for (uint64_t i = 0; i < count; ++i)
                        indices.push_back(start + uint32_t(i));
                    expanded = expandQuadStrip(indices, {}, factor);
                    expanded.metadata.update(
                        {{"source", "nonindexed_vertex_range"}, {"start_vertex", start}});
                    vertexOffset = 0u - start;
                }
            }
            stripPrimitives = expanded.indices.size() / factor;
            planned = stripPrimitives * instances;
            strip = expanded.metadata;
            strip.update({{"output_index_format", 42}, {"preserves_degenerate_primitives", true}});
            if (factor == 3)
                strip.update({{"triangles_per_instance", stripPrimitives}, {"odd_corner_order", {0, 2, 1}}});
            else
                strip["lines_per_instance"] = stripPrimitives;
            if (!expanded.indices.empty()) {
                if (expanded.indices.size() > UINT32_MAX / sizeof(uint32_t))
                    throw std::runtime_error("Expanded Quad strip exceeds D3D11 buffer size");
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = UINT(expanded.indices.size() * sizeof(uint32_t));
                desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
                const D3D11_SUBRESOURCE_DATA data{expanded.indices.data(), 0, 0};
                check(r.device_->CreateBuffer(&desc, &data, &stripBuffer), "Create expanded Quad indices");
            }
        }
    }
    uint64_t submit() {
        auto context = r.context_.Get();
        struct Restore {
            ID3D11DeviceContext *context;
            const std::map<uint32_t, uint32_t> &steps;
            std::array<ID3D11Buffer *, 32> buffers{};
            std::array<UINT, 32> strides{}, offsets{};
            Com<ID3D11Buffer> index;
            DXGI_FORMAT format{};
            UINT offset{};
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            bool strip;
            Restore(ID3D11DeviceContext *c, const std::map<uint32_t, uint32_t> &s, bool strip_)
                : context(c), steps(s), strip(strip_) {
                context->IAGetVertexBuffers(0, 32, buffers.data(), strides.data(), offsets.data());
                if (strip) {
                    context->IAGetIndexBuffer(&index, &format, &offset);
                    context->IAGetPrimitiveTopology(&topology);
                }
            }
            ~Restore() {
                for (const auto &[slot, step] : steps) {
                    (void)step;
                    context->IASetVertexBuffers(slot, 1, &buffers[slot], &strides[slot], &offsets[slot]);
                }
                if (strip) {
                    context->IASetIndexBuffer(index.Get(), format, offset);
                    context->IASetPrimitiveTopology(topology);
                }
                for (auto buffer : buffers)
                    if (buffer)
                        buffer->Release();
            }
        } restore(context, steps, isStrip);
        if (isStrip) {
            if (!planned)
                return 0;
            context->IASetIndexBuffer(stripBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            context->IASetPrimitiveTopology(factor == 3 ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                                        : D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        }
        uint64_t submissions = 0;
        for (uint64_t instance = 0; instance < instances; ++instance) {
            for (const auto &[slot, step] : steps) {
                const uint64_t extra = step ? (instance / step) * restore.strides[slot] : 0;
                const uint64_t value = uint64_t(restore.offsets[slot]) + extra;
                if (value > UINT32_MAX)
                    throw std::runtime_error(
                        "Split per-instance byte offset cannot be represented by IASetVertexBuffers");
                const auto offset = UINT(value);
                context->IASetVertexBuffers(slot, 1, &restore.buffers[slot], &restore.strides[slot], &offset);
            }
            const auto instanceCode = offsetSystemId(raw, uint32_t(instance), 8).bytes;
            const uint64_t vertices = isStrip ? stripPrimitives * factor : count - count % factor;
            for (uint64_t first = 0; first < vertices; first += factor) {
                const auto offset = isStrip ? vertexOffset : indexed ? 0u : uint32_t(first);
                const auto key = std::pair(uint32_t(instance), offset);
                if (!shaders.contains(key)) {
                    const auto code = offsetSystemId(instanceCode, offset, 6).bytes;
                    Com<ID3D11VertexShader> shader;
                    check(r.device_->CreateVertexShader(code.data(), code.size(), linkage.Get(), &shader),
                          "Create split Quad VS");
                    shaders[key] = std::move(shader);
                }
                std::vector<ID3D11ClassInstance *> pointers;
                for (const auto &object : classes)
                    pointers.push_back(object.Get());
                context->VSSetShader(shaders.at(key).Get(), pointers.empty() ? nullptr : pointers.data(),
                                     UINT(pointers.size()));
                const auto startInstance = parameters.value("start_instance", 0u);
                if (isStrip)
                    context->DrawIndexedInstanced(factor, 1, uint32_t(first),
                                                  parameters.value("base_vertex", int32_t(0)), startInstance);
                else if (indexed)
                    context->DrawIndexedInstanced(
                        factor, 1, parameters.at("start_index").get<uint32_t>() + uint32_t(first),
                        parameters.value("base_vertex", int32_t(0)), startInstance);
                else
                    context->DrawInstanced(factor, 1,
                                           parameters.at("start_vertex").get<uint32_t>() + uint32_t(first),
                                           startInstance);
                ++submissions;
            }
        }
        return submissions;
    }
    Json metadata() const {
        static constexpr const char *names[]{"",           "point_list",    "line_list",
                                             "line_strip", "triangle_list", "triangle_strip"};
        Json rates = Json::object();
        for (const auto &[slot, step] : steps)
            rates[std::to_string(slot)] = step;
        return {{"strategy", std::string("serialized_") + names[state.topology]},
                {"planned_submissions", planned},
                {"vertices_per_primitive", factor},
                {"resolved_parameters", parameters},
                {"indirect_arguments", indirect},
                {"strip_assembly", strip},
                {"per_instance_steps", rates},
                {"shader_variants", shaders.size()},
                {"vs_system_id_compensation", true},
                {"limitations",
                 {"Splitting changes GPU scheduling and shader-cache reuse; this is not a physical quad "
                  "invocation measurement.",
                  "This path handles VS-only lists/strips; final HS/DS/GS/SO output uses the separate native "
                  "capture path."}}};
    }
};
QuadSerial::QuadSerial(Replay &replay, const Event &event, const State &state)
    : state_(std::make_unique<QuadSerialState>(replay, event, state)) {}
QuadSerial::~QuadSerial() = default;
uint64_t QuadSerial::submit() { return state_->submit(); }
Json QuadSerial::metadata() const { return state_->metadata(); }
} // namespace flora
