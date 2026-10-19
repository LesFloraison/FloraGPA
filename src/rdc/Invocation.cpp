#include "Invocation.h"
#include <stdexcept>
#include <cstring>
#include <bit>
#include <cmath>
namespace flora {
namespace {
using Json = nlohmann::json;
Json triple(const rdcfixedarray<uint32_t, 3> &value) { return {value[0], value[1], value[2]}; }
} // namespace
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
Json rdcVertexPositionReference(const ActionDescription &action, const MeshFormat &mesh,
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
    uint64_t offset{};
    const auto relative = row * mesh.vertexByteStride;
    if (relative > instanceBytes || 16 > instanceBytes - relative ||
        !address(mesh.vertexByteOffset, relative, mesh.vertexByteSize, 16, offset))
        return unavailable("Post-VS position address exceeds the returned buffer");
    const auto bytes = read(mesh.vertexResourceId, offset, 16);
    if (bytes.size() != 16) return unavailable("Truncated post-VS position data");
    Json bits = Json::array(), values = Json::array();
    for (size_t i = 0; i < 4; ++i) {
        uint32_t word{}; memcpy(&word, bytes.data() + i * 4, 4); bits.push_back(word);
        const auto value = std::bit_cast<float>(word);
        values.push_back(std::isfinite(value) ? Json(value) : Json(std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf"));
    }
    result.update({{"status", "read"}, {"post_vertex", std::to_string(row)},
                   {"position_byte_offset", std::to_string(offset)},
                   {"value", {{"uintValue", bits}, {"floatValue", values}}}});
    return result;
}
} // namespace flora
