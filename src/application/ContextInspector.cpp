#include "ContextInspector.h"
#include "ApiCommands.h"
#include <algorithm>

namespace flora {
using Json = nlohmann::json;
Json contextJson(const ContextDescription &context, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    Json out{{"id", context.id},
             {"interface_version", context.version ? Json(*context.version) : Json(nullptr)},
             {"device", context.device},
             {"context_type", context.deferred ? "deferred" : "immediate"},
             {"creation_flags", context.flags ? Json(*context.flags) : Json(nullptr)},
             {"captured_pointer", context.pointer ? Json(*context.pointer) : Json(nullptr)}};
    if (!context.version) {
        out["provenance"] = "inferred_successful_staging_map_read";
        out["missing_fields"] = {"interface_version", "creation_flags", "captured_pointer"};
        out["evidence"] = Json::array();
        for (const auto &e : context.evidence) {
            checkCancellation(cancelled);
            out["evidence"].push_back({{"event", e.event},
                                       {"wire_type", e.wireType},
                                       {"resource", e.resource},
                                       {"subresource", e.subresource},
                                       {"device", e.device},
                                       {"hresult", e.hresult},
                                       {"map_type", e.mapType},
                                       {"data_identity", e.data},
                                       {"unmap_event", e.unmap}});
        }
    }
    return out;
}
Json inspectContexts(const Frame &frame, const CancelCheck &cancelled) {
    const auto &recovery = frame.contextRecovery(cancelled);
    Json out{{"contexts", Json::array()}, {"invalid", Json::array()}};
    Json report{{"contexts", Json::array()},
                {"issues", Json::array()},
                {"scope", "missing_context_kind_only_verified_context4_map_read"},
                {"assumption", "Captured successful calls obey D3D11 Map semantics; this does not "
                               "authenticate a modified capture."}};
    for (auto id : frame.entryOrder()) {
        checkCancellation(cancelled);
        auto &e = frame.entry(id);
        if (e.category != 5 || !contextVersion(e.type))
            continue;
        try {
            out["contexts"].push_back(contextJson(describeContext(frame, id), cancelled));
        } catch (const OperationCancelled &) {
            throw;
        } catch (const std::exception &error) {
            out["invalid"].push_back({{"id", id}, {"error", error.what()}});
        }
    }
    for (const auto &context : recovery.contexts) {
        auto value = contextJson(context, cancelled);
        report["contexts"].push_back(value);
        out["contexts"].push_back(value);
    }
    for (const auto &issue : recovery.issues) {
        checkCancellation(cancelled);
        Json item{{"context", issue.context}, {"reason", issue.reason}};
        if (issue.event)
            item["event"] = *issue.event;
        report["issues"].push_back(item);
    }
    out["recovery"] = std::move(report);
    checkCancellation(cancelled);
    return out;
}
Json inspectCommandList(const Frame &frame, Id id, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    frame.contextRecovery(cancelled);
    auto found = frame.entries().find(id);
    if (found == frame.entries().end() || found->second.category != 5 || found->second.type != 0x9a)
        throw std::runtime_error("Expected a command-list resource");
    auto raw = frame.payload(id);
    if (raw.size() != 16)
        throw std::runtime_error("Command-list resource requires a 16-byte payload");
    Reader r(raw);
    auto pointer = r.read<Id>(), parent = r.read<Id>();
    auto native = describeContext(frame, uint32_t(parent));
    std::optional<ContextDescription> owner;
    try {
        owner = describeContext(frame, parent);
    } catch (const std::exception &) {
    }
    auto truncated = Json::array();
    if (parent > UINT32_MAX)
        truncated.push_back("parent_context");
    return {{"id", id},
            {"captured_pointer", pointer},
            {"parent_context", parent},
            {"original_player_fields", {{"captured_pointer", pointer}, {"parent_context", uint32_t(parent)}}},
            {"original_player_truncated_fields", truncated},
            {"parent_context_type", owner ? (owner->deferred ? "deferred" : "immediate") : "unresolved"},
            {"interface_version", owner && owner->version ? Json(*owner->version) : Json(nullptr)},
            {"original_player_parent", contextJson(native, cancelled)},
            {"native_build_context_valid", native.deferred},
            {"native_build_finish_restore", true}};
}
Json inspectCommandLists(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    auto lists = Json::array(), executions = Json::array(), finishes = Json::array();
    std::map<Id, size_t> index;
    for (auto id : frame.entryOrder()) {
        checkCancellation(cancelled);
        auto &e = frame.entry(id);
        if (e.category != 5 || e.type != 0x9a)
            continue;
        auto info = inspectCommandList(frame, id, cancelled);
        info.update({{"recorded_events", Json::array()},
                     {"recorded_event_order", "capture_id_inventory_only"},
                     {"recorded_event_owner_domain", "original_player_uint32"},
                     {"original_builder_order", "registration_append_order_with_duplicates"},
                     {"registration_sequence_available", false}});
        index.emplace(id, lists.size());
        lists.push_back(std::move(info));
    }
    for (auto &[id, e] : frame.entries()) {
        checkCancellation(cancelled);
        if (e.category != 7)
            continue;
        if (finishCommandListVersion(e.type)) {
            auto row = inspectCommand(frame, id);
            if (!row.contains("command_list_finish"))
                throw std::runtime_error(row.value("error", "Invalid FinishCommandList record"));
            auto info = row["command_list_finish"];
            info["event"] = id;
            finishes.push_back(info);
        }
        auto t = e.type;
        bool ownerType = (t >= 0x31 && t <= 0x42) || (t >= 0x241 && t <= 0x243) ||
                         (t >= 0x245 && t <= 0x254) || t == 0x25e;
        if (ownerType) {
            auto raw = frame.payload(id);
            size_t offset = isDraw(t) ? 16 : 8;
            if (raw.size() < offset + 8)
                throw std::runtime_error("Command " + std::to_string(id) + " has a truncated owner field");
            Reader r(raw);
            r.skip(offset);
            auto owner = r.read<Id>();
            Id native = uint32_t(owner);
            if (index.contains(native)) {
                Json item{{"id", id}, {"type", t}, {"name", commandName(t)}};
                if (owner != native)
                    item.update({{"captured_owner", owner}, {"original_player_owner", native}});
                lists[index.at(native)]["recorded_events"].push_back(item);
            }
        }
        if (t == 0x41 || t == 0x30d1) {
            auto row = inspectCommand(frame, id);
            if (!row.contains("command_list"))
                throw std::runtime_error(row.value("error", "Invalid ExecuteCommandList record"));
            auto info = row["command_list"];
            auto operand = info["command_list_operand"].get<Id>();
            auto pointers = Json::array();
            for (auto &list : lists) {
                checkCancellation(cancelled);
                if (list["captured_pointer"] == operand)
                    pointers.push_back(list["id"]);
            }
            info.update({{"event", id},
                         {"id_candidate", index.contains(operand) ? Json(operand) : Json(nullptr)},
                         {"pointer_candidates", pointers},
                         {"resolved_command_list", nullptr}});
            executions.push_back(info);
        }
    }
    checkCancellation(cancelled);
    return {{"command_lists", std::move(lists)},
            {"execute_commands", std::move(executions)},
            {"finish_commands", std::move(finishes)},
            {"execution_supported", false},
            {"source", "recovered_resource_parent_and_erg_owner_fields"},
            {"limits",
             {"ExecuteCommandList operand identity has not been verified with an original list capture.",
              "Recorded events are listed by capture ID; this is not an expanded GPU execution timeline.",
              "Original native list registration preserves append order and duplicates; file inventory does "
              "not recover that callback sequence.",
              "Raw interface records, nested lists and capture-before-frame history are not reconstructed.",
              "This GPA build also contains capture-side command expansion; absent list resources do not "
              "prove the source application used only immediate contexts."}}};
}
} // namespace flora
