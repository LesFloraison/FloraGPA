#include "Invocation.h"
#include <stdexcept>
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
} // namespace flora
