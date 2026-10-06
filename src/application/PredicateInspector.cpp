#include "PredicateInspector.h"
#include "CaptureNames.h"
namespace flora {
using Json = nlohmann::json;
Json describePredicate(const Frame &frame, Id id) {
    auto desc = readPredicate(frame, id);
    Json out{{"id", id},
             {"type", 0x96},
             {"original", desc.original},
             {"device", desc.device},
             {"query_type", desc.type},
             {"query_name", desc.type == 5 ? "occlusion_predicate" : "so_overflow_predicate"},
             {"query_flags", desc.flags},
             {"initial_result_source", "native_player_empty_begin_end"}};
    const auto creations = auditPredicateCreations(frame);
    if (auto it = creations.creationEvents.find(id); it != creations.creationEvents.end()) {
        requirePredicateCreation(creations, it->second);
        out["creation_event"] = it->second;
        out["initial_result_source"] = "unissued_frame_time_creation";
    }
    Json names = Json::array();
    const auto catalog = capturedNames(frame);
    for (const auto &record : catalog["records"])
        if (record["resource_id"] == std::to_string(id) && record["name"] != "" &&
            std::find(names.begin(), names.end(), record["name"]) == names.end())
            names.push_back(record["name"]);
    if (!names.empty())
        out["debug_names"] = names;
    return out;
}
Json inspectPredicate(const Frame &frame, Replay &replay, Id id) {
    auto value = replay.readPredicateResult(id);
    return {{"resource_id", id},
            {"resource", describePredicate(frame, id)},
            {"value", value.value ? Json(*value.value) : Json(nullptr)},
            {"status", value.status},
            {"source", value.status == "replay_baseline" ? "native_player_empty_begin_end"
                                                       : "replayed_gpu_query"},
            {"captured_result_restored", false},
            {"bound", value.bound},
            {"predicate_value", value.predicateValue}};
}
} // namespace flora
