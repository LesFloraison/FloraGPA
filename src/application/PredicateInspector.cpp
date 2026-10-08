#include "PredicateInspector.h"
#include "CaptureNames.h"
namespace flora {
using Json = nlohmann::json;
Json describePredicate(const Frame &frame, Id id) {
    if (!frame.entries().contains(id)) {
        Json proofs = Json::array();
        for (const auto &[event, proof] : auditNormalizedPredication(frame))
            if (proof.resource == id)
                proofs.push_back({{"event", event}, {"witness_event", proof.witness},
                                  {"captured_value", proof.value}});
        if (!proofs.empty())
            return {{"id", id}, {"type", nullptr}, {"query_type", nullptr},
                    {"query_name", "unavailable"}, {"query_flags", nullptr},
                    {"descriptor_available", false}, {"condition_proofs", proofs},
                    {"initial_result_source", "captured_normalized_predication"}};
    }
    auto desc = readPredicate(frame, id);
    Json out{{"id", id},
             {"type", 0x96},
             {"original", desc.original},
             {"device", desc.device},
             {"query_type", desc.type},
             {"query_name", desc.type == 5 ? "occlusion_predicate" : "so_overflow_predicate"},
             {"query_flags", desc.flags},
             {"initial_result_source", "native_player_empty_begin_end"}};
    if (isStreamOverflowQuery(desc.type)) {
        out["query_name"] = "so_overflow_query_stream" + std::to_string((desc.type - 9) / 2);
        out["initial_result_source"] = "unissued_saved_stream_query";
        out["bindable_predicate"] = false;
    }
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
    Json result{{"resource_id", id},
            {"resource", describePredicate(frame, id)},
            {"value", value.value ? Json(*value.value) : Json(nullptr)},
            {"status", value.status},
            {"source", value.status == "captured_condition" ? "captured_normalized_predication"
                       : value.status == "replay_baseline" ? "native_player_empty_begin_end"
                                                       : "replayed_gpu_query"},
            {"captured_result_restored", false},
            {"bound", value.bound},
            {"predicate_value", value.predicateValue}};
    if (value.status == "captured_condition")
        result["condition_allows_execution"] = value.bound ? Json(value.predicateValue != 0) : Json(nullptr);
    return result;
}
} // namespace flora
