#include "Invocation.h"
#include <stdexcept>
#include <cstring>
namespace flora {
namespace {
using Json = nlohmann::json;
Json triple(const rdcfixedarray<uint32_t, 3> &value) { return {value[0], value[1], value[2]}; }
} // namespace
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
} // namespace flora
