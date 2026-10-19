#include "Invocation.h"
#include <stdexcept>
#include <cstring>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <QCryptographicHash>
namespace flora {
namespace {
using Json = nlohmann::json;
Json triple(const rdcfixedarray<uint32_t, 3> &value) { return {value[0], value[1], value[2]}; }
} // namespace
Json rdcTextureStoreRecord(const std::array<uint32_t, 32> &w) {
    if (w[31] || w[14] > 1 || w[27] > 1 || (w[12] & ~15U))
        throw std::runtime_error("Invalid texture store observer record");
    const auto resource = uint64_t(w[1]) | (uint64_t(w[2]) << 32);
    Json bits = Json::array(), values = Json::array();
    for (unsigned c = 0; c < 4; ++c) {
        bits.push_back(w[19 + c]);
        if (!(w[12] & (1U << c))) values.push_back(nullptr);
        else if (w[13] == uint32_t(VarType::Float)) {
            const auto f = std::bit_cast<float>(w[19 + c]);
            values.push_back(std::isfinite(f) ? Json(f) : Json(std::isnan(f) ? "nan" : f < 0 ? "-inf" : "inf"));
        } else if (w[13] == uint32_t(VarType::UInt)) values.push_back(w[19 + c]);
        else if (w[13] == uint32_t(VarType::SInt)) values.push_back(std::bit_cast<int32_t>(w[19 + c]));
        else values.push_back(nullptr); // Raw bits remain available for other scalar widths.
    }
    Json result{{"instruction", w[0]}, {"debug_step", w[30]}, {"rdc_resource", std::to_string(resource)},
        {"view_dimension", w[3]}, {"mip", w[4]}, {"view_first_slice", w[5]},
        {"view_extent", {w[6], w[7], w[8]}}, {"view_coordinates", {w[9], w[10], w[11]}},
        {"write_mask", w[12]}, {"value_type_enum", w[13]}, {"in_bounds", bool(w[14])},
        {"binding", {{"register", w[15]}, {"space", w[16]}, {"heap_type", w[17]}, {"descriptor_index", w[18]}}},
        {"value_bits", bits}, {"value", values}, {"view_format", w[23]},
        {"component_type_enum", w[24]}, {"component_byte_width", w[25]}, {"component_count", w[26]},
        {"operands_available", bool(w[27])},
        {"cache_byte_offset", std::to_string(uint64_t(w[28]) | (uint64_t(w[29]) << 32))},
        {"status", !w[27] ? "operands_unavailable" : !w[14] ? "out_of_bounds" : "observed_store"}};
    if (w[14]) {
        if (!resource || w[9] >= w[6] || w[10] >= w[7] || w[11] >= w[8])
            throw std::runtime_error("Texture store bounds disagree with the observer");
        uint64_t layer = w[5];
        uint32_t y = w[10];
        switch (w[3]) {
        case 2: y = 0; break; // D3D12_TEXTURE1D
        case 3: layer += w[10]; y = 0; break; // 1D array uses its second coordinate as layer.
        case 4: break;
        case 5: case 8: layer += w[11]; break;
        default: throw std::runtime_error("Unsupported texture store view dimension");
        }
        if (layer > UINT32_MAX) throw std::runtime_error("Texture store layer overflow");
        result["location"] = {{"x", w[9]}, {"y", y}, {"mip", w[4]}, {"layer", uint32_t(layer)}, {"sample", 0}};
    }
    return result;
}
Json rdcBufferInputSnapshot(const std::vector<RdcBufferInputRange> &ranges,
                             const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read,
                             uint64_t byteBudget) {
    Json result{{"status", "unavailable"}, {"ranges", Json::array()}};
    if (ranges.size() > 256) { result["reason"] = "Buffer input snapshot exceeds 256 views"; return result; }
    uint64_t total{};
    bool complete = true;
    for (const auto &range : ranges) {
        uint64_t resource{}; memcpy(&resource, &range.resource, sizeof resource);
        Json row{{"resource", "ResourceId::" + std::to_string(resource)},
                 {"offset", std::to_string(range.offset)}, {"bytes", std::to_string(range.size)},
                 {"status", "unavailable"}};
        if (!resource || !range.size || range.offset > range.bufferSize || range.size > range.bufferSize - range.offset)
            row["reason"] = "Input view is empty, unbound or outside its buffer";
        else if (range.size > byteBudget - total)
            row["reason"] = "Input snapshot byte budget exceeded";
        else {
            total += range.size;
            try {
                const auto bytes = read(range.resource, range.offset, range.size);
                if (bytes.size() != range.size) row["reason"] = "Incomplete input buffer read";
                else {
                    row["sha256"] = QCryptographicHash::hash(
                        QByteArrayView(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size())),
                        QCryptographicHash::Sha256).toHex().toStdString();
                    row["status"] = "read";
                }
            } catch (const std::exception &e) { row["reason"] = e.what(); }
        }
        complete &= row.at("status") == "read";
        result["ranges"].push_back(std::move(row));
    }
    result["status"] = ranges.empty() ? "no_buffer_views" : complete ? "read" : "incomplete";
    result["requested_bytes"] = std::to_string(total);
    return result;
}
Json rdcVertexOutputLayout(const ShaderReflection &shader, uint32_t stride) {
    Json result{{"status", "unavailable"}, {"scope", "RenderDoc 1.45 D3D12 VSOut stream-output layout"}};
    if (shader.stage != ShaderStage::Vertex || shader.outputSignature.empty() || shader.outputSignature.size() > 1024) {
        result["reason"] = "No bounded vertex output signature"; return result;
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < shader.outputSignature.size(); ++i)
        if (shader.outputSignature[i].systemValue == ShaderBuiltin::Position) order.insert(order.begin(), i);
        else order.push_back(i);
    uint64_t offset{}; unsigned positions{}; Json attributes = Json::array();
    for (const auto i : order) {
        const auto &s = shader.outputSignature[i];
        const bool position = s.systemValue == ShaderBuiltin::Position;
        positions += position;
        const auto count = position ? 4u : s.compCount;
        if (!count || count > 4 || s.stream || s.perPrimitiveRate || positions > 1) {
            result["reason"] = "Unsupported vertex output signature layout"; return result;
        }
        const auto type = s.varType == VarType::Float ? CompType::Float
                          : s.varType == VarType::UInt ? CompType::UInt
                          : s.varType == VarType::SInt ? CompType::SInt : CompType::Typeless;
        const bool supported = type != CompType::Typeless && (!position || s.compCount == 4);
        attributes.push_back({{"signature_index", i}, {"semantic", std::string(s.semanticName.c_str())},
            {"semantic_index", s.semanticIndex}, {"register", s.regIndex}, {"position", position},
            {"byte_offset", offset}, {"supported", supported},
            {"format", {{"compType", uint32_t(type)}, {"compByteWidth", 4}, {"compCount", count}}},
            {"reason", supported ? "" : "Only float32, uint32 and sint32 output components are decoded"}});
        offset += count * 4;
    }
    if (positions != 1) { result["reason"] = "No unique VS position output"; return result; }
    if (offset != stride) { result["reason"] = "Reflected output packing differs from the GPU vertex stride"; return result; }
    result.update({{"status", "mapped"}, {"stride", stride}, {"attributes", attributes}});
    return result;
}
Json rdcExecutedAction(const ActionDescription &action) {
    Json result{{"event_id", action.eventId},
                {"indirect", bool(action.flags & ActionFlags::Indirect)},
                {"generated_action_index", action.drawIndex},
                {"source", "RenderDoc action metadata; indirect arguments decoded during capture loading"},
                {"scope", "Invocation bounds use the indexed action. Nondeterministic indirect arguments may "
                          "differ on subsequent replays."}};
    if (action.flags & (ActionFlags::Dispatch | ActionFlags::MeshDispatch)) {
        result["kind"] = action.flags & ActionFlags::MeshDispatch ? "mesh_dispatch" : "dispatch";
        result["group_count"] = triple(action.dispatchDimension);
        result["group_size_override"] = triple(action.dispatchThreadsDimension);
        result["has_workgroups"] =
            action.dispatchDimension[0] && action.dispatchDimension[1] && action.dispatchDimension[2];
        result["has_group_size_override"] = action.dispatchThreadsDimension[0] &&
                                            action.dispatchThreadsDimension[1] &&
                                            action.dispatchThreadsDimension[2];
    } else if (action.flags & ActionFlags::Drawcall) {
        const bool indexed = bool(action.flags & ActionFlags::Indexed);
        result["kind"] = indexed ? "indexed_draw" : "draw";
        result[indexed ? "index_count" : "vertex_count"] = action.numIndices;
        result["instance_count"] = action.numInstances;
        result["first_instance"] = action.instanceOffset;
        if (indexed) {
            result["first_index"] = action.indexOffset;
            result["base_vertex"] = action.baseVertex;
        } else {
            result["first_vertex"] = action.vertexOffset;
        }
    } else {
        throw std::runtime_error("Selected action has no Draw/Dispatch invocation parameters");
    }
    return result;
}
Json rdcComputeInvocation(const ActionDescription &action, const ShaderReflection &shader,
                          const rdcfixedarray<uint32_t, 3> &group, const rdcfixedarray<uint32_t, 3> &thread) {
    if (!(action.flags & ActionFlags::Dispatch))
        throw std::runtime_error("Compute invocation requires a Dispatch action");
    Json global = Json::array();
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!action.dispatchDimension[axis])
            throw std::runtime_error("Selected Dispatch has zero workgroups; no compute invocation executed");
        if (!shader.dispatchThreadsDimension[axis])
            throw std::runtime_error("Compute shader thread-group size is unavailable");
        if (group[axis] >= action.dispatchDimension[axis])
            throw std::runtime_error("Selected compute group is outside the executed Dispatch dimensions");
        if (thread[axis] >= shader.dispatchThreadsDimension[axis])
            throw std::runtime_error("Selected compute thread is outside the shader thread-group dimensions");
        const uint64_t coordinate =
            uint64_t(group[axis]) * shader.dispatchThreadsDimension[axis] + thread[axis];
        if (coordinate > UINT32_MAX)
            throw std::runtime_error("Compute dispatch-thread coordinate exceeds uint32");
        global.push_back(uint32_t(coordinate));
    }
    auto result = rdcExecutedAction(action);
    result["group"] = triple(group);
    result["thread"] = triple(thread);
    result["shader_group_size"] = triple(shader.dispatchThreadsDimension);
    result["dispatch_thread"] = global;
    return result;
}
Json rdcVertexInvocation(const ActionDescription &action, const D3D12Pipe::InputAssembly &ia,
                         uint32_t vertex, uint32_t instance,
                         const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read) {
    if (!(action.flags & ActionFlags::Drawcall) || action.flags & ActionFlags::MeshDispatch)
        throw std::runtime_error("Vertex invocation requires a Draw action");
    if (vertex >= action.numIndices)
        throw std::runtime_error("Selected vertex ordinal is outside the executed Draw");
    if (instance >= action.numInstances)
        throw std::runtime_error("Selected vertex instance is outside the executed Draw");
    auto result = rdcExecutedAction(action);
    result.update({{"vertex_ordinal", vertex}, {"instance", instance}});
    int64_t actual{};
    if (action.flags & ActionFlags::Indexed) {
        const auto &ib = ia.indexBuffer;
        if (ib.resourceId == ResourceId::Null() || (ib.byteStride != 2 && ib.byteStride != 4))
            throw std::runtime_error("DX12 vertex invocation requires a bound 16/32-bit index buffer");
        const uint64_t relative = (uint64_t(action.indexOffset) + vertex) * ib.byteStride;
        if (relative > ib.byteSize || ib.byteStride > ib.byteSize - relative ||
            ib.byteOffset > UINT64_MAX - relative || ib.byteOffset + relative > UINT64_MAX - ib.byteStride)
            throw std::runtime_error("Selected vertex index exceeds the bound index range");
        const auto offset = ib.byteOffset + relative;
        const auto bytes = read(ib.resourceId, offset, ib.byteStride);
        if (bytes.size() != ib.byteStride) throw std::runtime_error("Truncated vertex invocation index data");
        uint32_t raw{}; memcpy(&raw, bytes.data(), ib.byteStride);
        if (IsStrip(ia.topology) && ia.indexStripCutValue && raw == ia.indexStripCutValue)
            throw std::runtime_error("Selected index is a strip-cut marker, not a vertex invocation");
        actual = int64_t(raw) + action.baseVertex;
        uint64_t resource{}; memcpy(&resource, &ib.resourceId, sizeof(resource));
        result.update({{"raw_index", raw}, {"index_byte_offset", std::to_string(offset)},
            {"index_byte_stride", ib.byteStride}, {"index_resource", "ResourceId::" + std::to_string(resource)}});
    } else actual = int64_t(action.vertexOffset) + vertex;
    if (actual < 0 || uint64_t(actual) > UINT32_MAX)
        throw std::runtime_error("Actual vertex index is outside uint32");
    result["actual_vertex_index"] = uint32_t(actual);
    result["index_source"] = action.flags & ActionFlags::Indexed ? "replayed_index_buffer" : "draw_vertex_offset";
    return result;
}
Json rdcVertexInputSnapshot(const ActionDescription &action, const D3D12Pipe::InputAssembly &ia,
                            uint32_t vertex, uint32_t instance,
                            const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read) {
    Json result{{"status", "unavailable"}, {"attributes", Json::array()}};
    if (ia.layouts.size() > 128) { result["reason"] = "Input layout exceeds snapshot limit"; return result; }
    try { result["invocation"] = rdcVertexInvocation(action, ia, vertex, instance, read); }
    catch (const std::exception &e) { result["reason"] = e.what(); return result; }
    std::map<uint32_t, std::optional<uint64_t>> next;
    bool complete = true;
    for (const auto &layout : ia.layouts) {
        Json entry{{"semantic", std::string(layout.semanticName.c_str())}, {"semantic_index", layout.semanticIndex},
                   {"slot", layout.inputSlot}, {"status", "unavailable"}};
        const auto &f = layout.format;
        const bool regular = f.type == ResourceFormatType::Regular && f.compCount && f.compCount <= 4 &&
                             (f.compByteWidth == 1 || f.compByteWidth == 2 || f.compByteWidth == 4 || f.compByteWidth == 8);
        const auto previous = next.find(layout.inputSlot);
        const std::optional<uint64_t> offset = layout.byteOffset == D3D12Pipe::Layout::TightlyPacked
            ? (previous == next.end() ? std::optional<uint64_t>(0) : previous->second)
            : std::optional<uint64_t>(layout.byteOffset);
        const uint64_t length = uint64_t(f.compCount) * f.compByteWidth;
        next[layout.inputSlot] = regular && offset ? std::optional<uint64_t>(*offset + length) : std::nullopt;
        if (!regular || !offset) entry["reason"] = "Packed input size or append offset is unavailable";
        else if (layout.inputSlot >= ia.vertexBuffers.size()) entry["reason"] = "Input slot is unbound";
        else {
            const auto &vb = ia.vertexBuffers[layout.inputSlot];
            const uint64_t element = layout.perInstance
                ? uint64_t(action.instanceOffset) + (layout.instanceDataStepRate ? instance / layout.instanceDataStepRate : 0)
                : result.at("invocation").at("actual_vertex_index").get<uint32_t>();
            uint64_t id{}; memcpy(&id, &vb.resourceId, 8);
            entry.update({{"resource", "ResourceId::" + std::to_string(id)}, {"element", std::to_string(element)}});
            if (vb.resourceId == ResourceId::Null() || (vb.byteStride && element > (UINT64_MAX - *offset) / vb.byteStride))
                entry["reason"] = "Input binding is null or address overflows";
            else {
                const uint64_t relative = element * vb.byteStride + *offset;
                if (relative > vb.byteSize || length > vb.byteSize - relative ||
                    vb.byteOffset > UINT64_MAX - relative || vb.byteOffset + relative > UINT64_MAX - length)
                    entry["reason"] = "Selected input exceeds its binding";
                else {
                    entry["byte_offset"] = std::to_string(vb.byteOffset + relative);
                    const auto bytes = read(vb.resourceId, vb.byteOffset + relative, length);
                    if (bytes.size() != length) entry["reason"] = "Truncated selected input data";
                    else {
                        entry["status"] = "read"; entry["bytes"] = Json::array();
                        for (const auto byte : bytes) entry["bytes"].push_back(byte);
                    }
                }
            }
        }
        complete = complete && entry.at("status") == "read";
        result["attributes"].push_back(std::move(entry));
    }
    result["status"] = complete ? "read" : "partial";
    return result;
}
static Json vertexReference(const ActionDescription &action, const MeshFormat &mesh, const ShaderReflection *shader,
                                uint32_t vertex, uint32_t instance,
                                const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read) {
    Json result{{"status", "unavailable"}, {"vertex_ordinal", vertex}, {"instance", instance},
                {"event_id", action.eventId}, {"interpretation", "float"},
                {"method", "RenderDoc GetPostVSData VSOut, view 0; selected position buffer read"},
                {"scope", "Separate instrumented VS replay. Matching position does not validate other "
                          "outputs, intermediate instructions or input/replay stability."}};
    const auto unavailable = [&](const char *reason) { result["reason"] = reason; return result; };
    if (!(action.flags & ActionFlags::Drawcall) || action.flags & ActionFlags::MeshDispatch ||
        vertex >= action.numIndices || instance >= action.numInstances)
        return unavailable("Selected invocation is outside a vertex Draw");
    if (mesh.vertexResourceId == ResourceId::Null())
        return unavailable("No post-VS vertex buffer was returned");
    if (!mesh.unproject || mesh.format.type != ResourceFormatType::Regular ||
        mesh.format.compType != CompType::Float || mesh.format.compCount != 4 ||
        mesh.format.compByteWidth != 4 || mesh.vertexByteStride < 16)
        return unavailable("Post-VS data does not identify a float4 position");
    if (mesh.numIndices != action.numIndices)
        return unavailable("Post-VS invocation count differs from the selected Draw");
    // RenderDoc 1.45 D3D12 VSOut stores equally sized instance blocks and
    // reports the whole allocation size. Do not let a bad index reach the next
    // instance, or accept its internally truncated uint32 instance stride.
    if (mesh.vertexByteSize % action.numInstances)
        return unavailable("Post-VS instance block sizes are inconsistent");
    const auto instanceBytes = mesh.vertexByteSize / action.numInstances;
    if ((action.numInstances > 1 && !(action.flags & ActionFlags::Instanced)) ||
        mesh.vertexByteOffset != instanceBytes * instance)
        return unavailable("Post-VS instance offset differs from the selected Draw");
    const auto address = [](uint64_t start, uint64_t relative, uint64_t size, uint64_t count,
                            uint64_t &out) {
        if (start > UINT64_MAX - relative) return false;
        out = start + relative;
        return out <= size && count <= size - out;
    };
    uint64_t row = vertex;
    if (mesh.indexByteStride) {
        if (mesh.indexResourceId == ResourceId::Null() ||
            (mesh.indexByteStride != 2 && mesh.indexByteStride != 4))
            return unavailable("Post-VS index mapping is missing or unsupported");
        uint64_t offset{};
        if (!address(mesh.indexByteOffset, uint64_t(vertex) * mesh.indexByteStride,
                     mesh.indexByteSize, mesh.indexByteStride, offset))
            return unavailable("Post-VS index address exceeds the returned buffer");
        const auto bytes = read(mesh.indexResourceId, offset, mesh.indexByteStride);
        if (bytes.size() != mesh.indexByteStride)
            return unavailable("Truncated post-VS index data");
        uint32_t raw{}; memcpy(&raw, bytes.data(), mesh.indexByteStride);
        const int64_t adjusted = int64_t(raw) + mesh.baseVertex;
        if (adjusted < 0 || uint64_t(adjusted) > UINT32_MAX)
            return unavailable("Post-VS remapped vertex is outside uint32");
        row = uint64_t(adjusted);
        result.update({{"post_index", raw}, {"post_base_vertex", mesh.baseVertex},
                       {"index_byte_offset", std::to_string(offset)}});
    } else if (action.flags & ActionFlags::Indexed) {
        return unavailable("Indexed Draw has no post-VS index mapping");
    }
    const auto layout = shader ? rdcVertexOutputLayout(*shader, mesh.vertexByteStride) : Json();
    const bool allOutputs = layout.is_object() && layout.value("status", "") == "mapped";
    const uint64_t readBytes = allOutputs ? mesh.vertexByteStride : 16;
    if (shader) result["output_layout"] = layout;
    uint64_t offset{};
    const auto relative = row * mesh.vertexByteStride;
    if (relative > instanceBytes || readBytes > instanceBytes - relative ||
        !address(mesh.vertexByteOffset, relative, mesh.vertexByteSize, readBytes, offset))
        return unavailable("Post-VS position address exceeds the returned buffer");
    const auto bytes = read(mesh.vertexResourceId, offset, readBytes);
    if (bytes.size() != readBytes) return unavailable("Truncated post-VS vertex data");
    Json bits = Json::array(), values = Json::array();
    for (size_t i = 0; i < 4; ++i) {
        uint32_t word{}; memcpy(&word, bytes.data() + i * 4, 4); bits.push_back(word);
        const auto value = std::bit_cast<float>(word);
        values.push_back(std::isfinite(value) ? Json(value) : Json(std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf"));
    }
    result.update({{"status", "read"}, {"post_vertex", std::to_string(row)},
                   {"position_byte_offset", std::to_string(offset)},
                   {"value", {{"uintValue", bits}, {"floatValue", values}}}});
    if (allOutputs) {
        result["method"] = "RenderDoc GetPostVSData VSOut, view 0; one selected output vertex";
        result["outputs"] = Json::array();
        for (const auto &attribute : layout.at("attributes")) {
            auto output = attribute; output["status"] = "unavailable";
            if (attribute.at("supported") == true) {
                const auto start = attribute.at("byte_offset").get<size_t>();
                const auto count = attribute.at("format").at("compCount").get<size_t>();
                const auto type = attribute.at("format").at("compType").get<uint32_t>();
                Json words = Json::array();
                for (size_t c = 0; c < count; ++c) {
                    uint32_t word{}; memcpy(&word, bytes.data() + start + 4 * c, 4); words.push_back(word);
                }
                output.update({{"status", "read"}, {"interpretation", type == uint32_t(CompType::Float) ? "float" : type == uint32_t(CompType::UInt) ? "uint" : "sint"},
                    {"value", {{"uintValue", words}}}, {"buffer_byte_offset", std::to_string(offset + start)}});
            }
            result["outputs"].push_back(std::move(output));
        }
    }
    return result;
}
Json rdcVertexPositionReference(const ActionDescription &action, const MeshFormat &mesh,
                                uint32_t vertex, uint32_t instance,
                                const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read) {
    return vertexReference(action, mesh, nullptr, vertex, instance, read);
}
Json rdcVertexOutputReference(const ActionDescription &action, const MeshFormat &mesh, const ShaderReflection &shader,
                              uint32_t vertex, uint32_t instance,
                              const std::function<bytebuf(ResourceId, uint64_t, uint64_t)> &read) {
    return vertexReference(action, mesh, &shader, vertex, instance, read);
}
} // namespace flora
