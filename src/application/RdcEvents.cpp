#include "RdcEvents.h"
#include "ReplayRequest.h"
#include <charconv>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>

namespace flora {
using Json = nlohmann::json;
namespace {
// Public ActionFlags values from RenderDoc 1.45 replay_enums.h.
constexpr uint32_t clear = 1, draw = 2, dispatch = 4, meshDispatch = 8, push = 0x40, copy = 0x400, resolve = 0x800,
                   mips = 0x1000;
const std::map<std::string, uint32_t> apiFlags{{"ClearDepthStencilView", clear},
                                               {"ClearRenderTargetView", clear},
                                               {"ClearUnorderedAccessViewUint", clear},
                                               {"ClearUnorderedAccessViewFloat", clear},
                                               {"CopyResource", copy},
                                               {"CopySubresourceRegion", copy},
                                               {"CopyStructureCount", copy},
                                               {"ResolveSubresource", resolve},
                                               {"GenerateMips", mips},
                                               {"UpdateSubresource", copy},
                                               {"MapCapturedWrites", 0}};
const std::set<std::string> draws{"Draw",
                                  "DrawAuto",
                                  "DrawIndexed",
                                  "DrawInstanced",
                                  "DrawIndexedInstanced",
                                  "DrawInstancedIndirect",
                                  "DrawIndexedInstancedIndirect"};
const std::map<std::string, std::string> cpuCalls{
    {"UpdateSubresource", "ID3D11DeviceContext::UpdateSubresource"},
    {"MapCapturedWrites", "ID3D11DeviceContext::Unmap"}};
struct Scope {
    uint64_t gpa{};
    uint32_t marker{}, flag{};
    std::string name, kind;
    std::set<uint32_t> events, api;
};
uint32_t number(const Json &value) {
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && value < 0) || value > UINT32_MAX)
        throw std::runtime_error("RenderDoc event/flag value exceeds uint32 range");
    return value.get<uint32_t>();
}
} // namespace
Json indexRdcEvents(const Json &roots, const Json &nativeEvents) {
    static const std::regex marker("^GPA (API )?([0-9]+): ([A-Za-z][A-Za-z0-9]*)$");
    if (!roots.is_array() || !nativeEvents.is_object())
        throw std::runtime_error("Invalid RenderDoc action/event collection");
    Json actions = Json::array(), selected = Json::object(), reverse = Json::object(),
         commands = Json::object();
    std::map<uint64_t, std::vector<std::unique_ptr<Scope>>> groups;
    std::vector<uint64_t> order;
    size_t visited = 0;
    std::function<void(const Json &, const std::string &, Scope *, unsigned)> walk;
    walk = [&](const Json &nodes, const std::string &parent, Scope *scope, unsigned depth) {
        if (!nodes.is_array() || depth > 128)
            throw std::runtime_error("RenderDoc action tree exceeds traversal bounds");
        for (const auto &action : nodes) {
            if (++visited > 1000000)
                throw std::runtime_error("RenderDoc action count exceeds traversal bounds");
            const auto eid = number(action.at("eventId")), flags = number(action.at("flags"));
            const auto name = action.at("customName").get<std::string>();
            if (name.size() > 1048576)
                throw std::runtime_error("RenderDoc action name exceeds traversal bounds");
            std::smatch match;
            auto current = scope;
            if (std::regex_match(name, match, marker) && (flags & push)) {
                current = nullptr;
                const bool api = match[1].matched;
                const auto command = match[3].str();
                const bool known =
                    api ? apiFlags.contains(command)
                        : draws.contains(command) || command == "Dispatch" || command == "DispatchIndirect";
                if (known) {
                    const auto id = match[2].str();
                    uint64_t gpa{};
                    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), gpa);
                    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size())
                        throw std::runtime_error("GPA marker identifier exceeds uint64 range");
                    if (!groups.contains(gpa))
                        order.push_back(gpa);
                    auto value = std::make_unique<Scope>();
                    value->gpa = gpa;
                    value->marker = eid;
                    value->name = command;
                    value->kind = api ? "api" : "draw";
                    value->flag = api ? apiFlags.at(command) : draws.contains(command) ? draw : dispatch;
                    current = value.get();
                    groups[gpa].push_back(std::move(value));
                }
            } else if (name.starts_with("GPA ")) {
                current = nullptr;
            } else if (current && (flags & current->flag)) {
                current->events.insert(eid);
            }
            if (current && cpuCalls.contains(current->name)) {
                for (const auto &event : action.value("events", Json::array())) {
                    const auto id = number(event.at("eventId"));
                    const auto native = nativeEvents.find(std::to_string(id));
                    if (id > current->marker && native != nativeEvents.end() &&
                        native->at("name") == cpuCalls.at(current->name)) {
                        current->events.insert(id);
                        current->api.insert(id);
                    }
                }
            }
            if (flags & (draw | dispatch))
                actions.push_back({{"eid", eid},
                                   {"name", name},
                                   {"marker", parent},
                                   {"indices", action.at("numIndices")},
                                   {"instances", action.at("numInstances")},
                                   {"flags", action.at("flags_text")}});
            walk(action.at("children"), name.empty() ? parent : name, current, depth + 1);
        }
    };
    walk(roots, "", nullptr, 0);
    for (auto gpa : order) {
        const auto &values = groups.at(gpa);
        std::set<uint32_t> eids, api;
        Json markers = Json::array();
        for (const auto &value : values) {
            eids.insert(value->events.begin(), value->events.end());
            api.insert(value->api.begin(), value->api.end());
            markers.push_back(value->marker);
        }
        const std::string status = values.size() != 1 ? "duplicate_markers"
                                   : eids.empty()     ? "unrepresented"
                                                      : "mapped";
        Json command{{"gpa_event", gpa},        {"name", values[0]->name},
                     {"kind", values[0]->kind}, {"event_ids", eids},
                     {"status", status},        {"selectable", status == "mapped" && eids.size() == 1},
                     {"marker_eids", markers},  {"api_event_ids", api}};
        commands[std::to_string(gpa)] = command;
        if (status == "mapped") {
            for (auto eid : eids)
                reverse[std::to_string(eid)] = command;
            if (eids.size() == 1)
                selected[std::to_string(gpa)] = *eids.begin();
        }
    }
    return {{"actions", actions},
            {"gpa_event_map", selected},
            {"gpa_command_map", commands},
            {"reverse", reverse}};
}
Json indexDx12RdcEvents(const Json &roots, const Json &nativeEvents) {
    static const std::regex marker(
        "^Flora DX12 execution ([0-9]+) record ([0-9]+) submission ([0-9]+) queue ([0-9]+) list ([0-9]+) "
        "generation ([0-9]+) ordinal ([0-9]+): (ID3D12GraphicsCommandList[0-9]*[A-Za-z][A-Za-z0-9]*)$");
    static const std::regex method("^ID3D12GraphicsCommandList[0-9]*([A-Za-z][A-Za-z0-9]*)$");
    if (!roots.is_array() || !nativeEvents.is_object())
        throw std::runtime_error("Invalid DX12 action/event collection");
    struct Execution {
        Json identity;
        std::string method;
        uint32_t marker{};
        std::set<uint32_t> events, api;
    };
    std::map<std::string, std::vector<std::unique_ptr<Execution>>> groups;
    std::map<uint32_t, std::string> actionKinds;
    Json actions = Json::array(), selected = Json::object(), commands = Json::object(),
         reverse = Json::object(), records = Json::object();
    size_t visited{};
    std::function<void(const Json &, Execution *, unsigned)> walk;
    walk = [&](const Json &nodes, Execution *scope, unsigned depth) {
        if (!nodes.is_array() || depth > 128)
            throw std::runtime_error("DX12 action tree exceeds traversal bounds");
        for (const auto &action : nodes) {
            if (++visited > 1000000)
                throw std::runtime_error("Too many DX12 analysis actions");
            const auto eid = number(action.at("eventId")), flags = number(action.at("flags"));
            const auto name = action.at("customName").get<std::string>();
            if (name.size() > 1048576)
                throw std::runtime_error("DX12 action name exceeds traversal bounds");
            auto current = scope;
            std::smatch match;
            if ((flags & push) && std::regex_match(name, match, marker)) {
                auto entry = std::make_unique<Execution>();
                entry->marker = eid;
                constexpr const char *keys[]{"execution", "source_record", "submission",  "queue",
                                             "list",      "generation",    "list_ordinal"};
                for (size_t i = 0; i < std::size(keys); ++i) {
                    const auto text = match[i + 1].str();
                    uint64_t value{};
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                        (!value && i != 6))
                        throw std::runtime_error("Invalid uint64 DX12 execution identity");
                    entry->identity[keys[i]] = std::to_string(value);
                }
                entry->identity["backend"] = "dx12";
                entry->identity["name"] = match[8].str();
                const auto command = match[8].str();
                std::smatch methodMatch;
                std::regex_match(command, methodMatch, method);
                entry->method = methodMatch[1].str();
                current = entry.get();
                groups[entry->identity["execution"].get<std::string>()].push_back(std::move(entry));
            } else if (name.starts_with("Flora DX12 "))
                current = nullptr;
            if (current) {
                for (const auto &event : action.value("events", Json::array())) {
                    const auto id = number(event.at("eventId"));
                    const auto native = nativeEvents.find(std::to_string(id));
                    if (id > current->marker && native != nativeEvents.end() &&
                        native->at("name").get<std::string>().ends_with("::" + current->method)) {
                        current->api.insert(id);
                        // ExecuteIndirect has API begin/end wrappers as well as
                        // generated Draw/Dispatch actions. Only the latter are
                        // executable selections; preserve the wrappers separately.
                        if (current->method != "ExecuteIndirect")
                            current->events.insert(id);
                    }
                }
                if (flags & (draw | dispatch | meshDispatch | clear | copy | resolve))
                    current->events.insert(eid);
            }
            if (flags & (draw | dispatch | meshDispatch)) {
                actionKinds[eid] = flags & meshDispatch ? "mesh_dispatch" : flags & dispatch ? "dispatch" : "draw";
                actions.push_back({{"eid", eid},
                                   {"name", name},
                                   {"indices", action.at("numIndices")},
                                   {"instances", action.at("numInstances")},
                                   {"dispatch_groups", action.value("dispatchDimension", Json(nullptr))},
                                   {"dispatch_threads", action.value("dispatchThreadsDimension", Json(nullptr))},
                                   {"flags", action.at("flags_text")}});
            }
            walk(action.at("children"), current, depth + 1);
        }
    };
    walk(roots, nullptr, 0);
    for (const auto &[execution, entries] : groups) {
        std::set<uint32_t> eids, api;
        Json markers = Json::array();
        for (const auto &entry : entries) {
            eids.insert(entry->events.begin(), entry->events.end());
            api.insert(entry->api.begin(), entry->api.end());
            markers.push_back(entry->marker);
        }
        const auto status = entries.size() != 1 ? "duplicate_markers"
                            : eids.empty()      ? "unrepresented"
                                                : "mapped";
        auto command = entries.front()->identity;
        command.update({{"status", status},
                        {"event_ids", eids},
                        {"api_event_ids", api},
                        {"marker_eids", markers},
                        {"selectable", entries.size() == 1 && eids.size() == 1}});
        commands[execution] = command;
        const auto record = command["source_record"].get<std::string>();
        if (!records.contains(record))
            records[record] = Json::array();
        records[record].push_back(execution);
        if (entries.size() == 1) {
            uint32_t ordinal{};
            for (const auto eid : eids) {
                auto provenance = command;
                if (command.at("name") == "ID3D12GraphicsCommandListExecuteIndirect")
                    provenance.update({{"indirect_action", ordinal++}, {"indirect_action_count", eids.size()},
                                       {"indirect_action_kind", actionKinds.contains(eid) ? actionKinds.at(eid) : "unknown"}});
                reverse[std::to_string(eid)] = std::move(provenance);
            }
            if (eids.size() == 1)
                selected[execution] = *eids.begin();
        }
    }
    return {{"backend", "dx12"},
            {"actions", actions},
            {"execution_map", selected},
            {"execution_commands", commands},
            {"record_executions", records},
            {"reverse", reverse},
            {"gpa_event_map", Json::object()},
            {"gpa_command_map", Json::object()}};
}
Json selectDx12RdcExecution(const Json &mapping, const std::string &execution, const Json &indirectAction) {
    const auto &commands = mapping.at("execution_commands");
    const auto found = commands.find(execution);
    if (found == commands.end() || found->value("status", "") == "duplicate_markers")
        throw std::runtime_error("Selected DX12 execution has no unique provenance marker");
    const auto &events = found->at("event_ids");
    const bool indirect = found->at("name") == "ID3D12GraphicsCommandListExecuteIndirect";
    if (!indirect && !indirectAction.is_null())
        throw std::runtime_error("Indirect action selection requires ExecuteIndirect");
    if (events.empty())
        throw std::runtime_error(indirect ? "Selected indirect call generated no Draw/Dispatch action"
                                          : "Selected capture event has no mapped replay action");
    if (!indirect && events.size() != 1)
        throw std::runtime_error("Selected capture event has no uniquely mapped executed action");
    if (events.size() > 1 && indirectAction.is_null())
        throw std::runtime_error("Selected execution has " + std::to_string(events.size()) +
            " actions; choose an explicit zero-based indirect action");
    const auto ordinal = indirectAction.is_null() ? 0 : number(indirectAction);
    if (ordinal >= events.size()) throw std::runtime_error("Indirect action is outside the captured expansion");
    const auto eid = number(events.at(ordinal));
    auto result = rdcProvenance(eid, mapping.at("reverse"));
    if (result.value("execution", "") != execution || result.value("mapping_status", "") != "mapped")
        throw std::runtime_error("Selected replay action does not belong to the DX12 execution");
    result["eid"] = eid;
    result["selection_scope"] = indirect
        ? "Action ordinal within the indexed ExecuteIndirect expansion; later replay arguments may change"
        : "Unique indexed replay action for the original execution";
    return result;
}
Json rdcProvenance(uint32_t event, const Json &reverse) {
    const auto found = reverse.find(std::to_string(event));
    if (found != reverse.end() && found->contains("execution")) {
        auto result = *found;
        result["mapping_status"] = "mapped";
        result["gpa_event"] = nullptr;
        if (result.contains("indirect_action") && result.value("indirect_action_kind", "unknown") != "unknown")
            result["selectable"] = true;
        return result;
    }
    if (found != reverse.end())
        return {{"gpa_event", found->at("gpa_event")},
                {"gpa_command", found->at("name")},
                {"gpa_command_kind", found->at("kind")},
                {"mapping_status", "mapped"}};
    return {{"gpa_event", nullptr},
            {"gpa_command", nullptr},
            {"gpa_command_kind", nullptr},
            {"mapping_status", "unmapped_replay_action"}};
}
namespace {
constexpr const char *queryPolicyPrefix = "Flora DX12 query isolation v1: ";
uint64_t policyNumber(const Json &value) {
    const auto &text = value.get_ref<const std::string &>(); uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || std::to_string(result) != text)
        throw std::runtime_error("Invalid uint64 query-isolation identity/range");
    return result;
}
void validateQueryPolicy(const Json &p) {
    if (p.at("schema") != 1 || p.at("policy") != "isolate_readback_outputs" || p.at("frame_complete") != false ||
        !p.at("after").is_boolean() || !p.at("resources").is_array() || p.at("resources").size() > 4096 ||
        !p.at("resolves").is_array() || p.at("resolves").size() > 16384)
        throw std::runtime_error("Invalid query-isolation capture policy");
    const auto selected = policyNumber(p.at("selected_execution"));
    if (!selected) throw std::runtime_error("Missing query-isolation execution boundary");
    std::map<uint64_t, uint64_t> widths;
    for (const auto &resource : p.at("resources")) {
        const auto creation = policyNumber(resource.at("creation")), width = policyNumber(resource.at("width"));
        if (!creation || !width || !policyNumber(resource.at("object")) || !widths.emplace(creation, width).second)
            throw std::runtime_error("Invalid or duplicate isolated query resource");
    }
    std::set<uint64_t> executions;
    for (const auto &entry : p.at("resolves")) {
        const auto execution = policyNumber(entry.at("execution")), creation = policyNumber(entry.at("creation"));
        const auto offset = policyNumber(entry.at("offset")), bytes = policyNumber(entry.at("bytes"));
        if (!execution || execution > selected || (execution == selected && !p.at("after").get<bool>()) ||
            !executions.insert(execution).second || !widths.contains(creation) || !policyNumber(entry.at("source_record")) ||
            !policyNumber(entry.at("queue")) || !policyNumber(entry.at("submission")) ||
            !bytes || offset > widths.at(creation) || bytes > widths.at(creation) - offset || !entry.at("ranges").is_array())
            throw std::runtime_error("Invalid isolated query resolve provenance");
        uint64_t next = offset; bool missing = false;
        for (const auto &range : entry.at("ranges")) {
            const auto at = policyNumber(range.at("offset")), count = policyNumber(range.at("bytes"));
            if (at != next || !count || count > offset + bytes - next || !range.at("available_from_replayed_queries").is_boolean())
                throw std::runtime_error("Invalid isolated query destination ranges");
            next += count; missing |= !range.at("available_from_replayed_queries").get<bool>();
        }
        if (next != offset + bytes || !missing) throw std::runtime_error("Incomplete missing-query range coverage");
    }
}
}
std::string rdcQueryIsolationMarker(const Json &policy) {
    validateQueryPolicy(policy);
    const auto marker = std::string(queryPolicyPrefix) + policy.dump();
    if (marker.size() > 1048576) throw std::runtime_error("Query-isolation marker exceeds one MiB");
    return marker;
}
Json readRdcQueryIsolation(const Json &roots) {
    Json result; size_t visited{};
    std::function<void(const Json &, unsigned)> walk = [&](const Json &nodes, unsigned depth) {
        if (!nodes.is_array() || depth > 128) throw std::runtime_error("Invalid query-policy action tree");
        for (const auto &node : nodes) {
            if (++visited > 1000000) throw std::runtime_error("Query-policy action limit exceeded");
            const auto name = node.at("customName").get<std::string>();
            if (name.starts_with("Flora DX12 query isolation")) {
                if (!result.is_null() || !name.starts_with(queryPolicyPrefix) || name.size() > 1048576)
                    throw std::runtime_error("Ambiguous or unsupported query-isolation capture marker");
                result = Json::parse(name.substr(std::char_traits<char>::length(queryPolicyPrefix)),
                    [](int depth, Json::parse_event_t, Json &) {
                        if (depth > 64) throw std::runtime_error("Query-isolation marker nesting limit exceeded");
                        return true;
                    });
                validateQueryPolicy(result);
            }
            walk(node.at("children"), depth + 1);
        }
    };
    walk(roots, 0); return result;
}
void requireRdcQueryIsolation(const Json &policy, const Json &job) {
    const bool requested = job.value("isolate_missing_queries", false);
    if (job.contains("request_identity") && parseReplayIdentity(job.at("request_identity")).isolateMissingQueries != requested)
        throw std::runtime_error("Analysis request query policy does not match its identity");
    if (requested != !policy.is_null()) throw std::runtime_error("Analysis query policy does not match the embedded capture marker");
    if (!requested) return;
    validateQueryPolicy(policy);
    const auto action = job.at("action").get<std::string>();
    if (action == "resource-history" || action == "history") {
        if (!policyNumber(job.at("execution")) || !policyNumber(job.at("resource")) ||
            !policyNumber(job.at("resource_creation")))
            throw std::runtime_error("Isolated history requires explicit nonzero DX12 identities");
        if (action == "resource-history")
            return; // GetUsage inspects recorded operations; it never reads query-result bytes.
    }
    if ((action == "pipeline" || rdcShaderDebugAction(action)) &&
        (!job.contains("execution") || !policyNumber(job.at("execution"))))
        throw std::runtime_error("Isolated execution analysis requires an explicit nonzero DX12 execution");
    if (action != "inventory" && action != "texture" && action != "pipeline" && action != "history" && !rdcShaderDebugAction(action))
        throw std::runtime_error("Unsupported analysis of isolated query replay");
    // Query readbacks remain excluded as a whole: later completed writes must
    // not be mistaken for valid bytes at an earlier selected analysis event.
    if (!job.value("resource", Json()).is_null()) for (const auto &resource : policy.at("resources"))
        if (job.at("resource") == resource.at("object") &&
            (!job.contains("resource_creation") || job.at("resource_creation") == resource.at("creation")))
            throw std::runtime_error("Analysis cannot read isolated query output resources");
}
void verifyRdcQueryResolveExpansion(const Json &command, const Json &entry,
                                    const Json &operations, const std::string &resourceId) {
    const auto fail=[] { throw std::runtime_error("Isolated query resolve expansion does not match captured provenance/ranges"); };
    if (command.value("status","") != "mapped" ||
        command.value("name","") != "ID3D12GraphicsCommandListResolveQueryData" ||
        command.at("marker_eids").size() != 1) fail();
    for (const auto key : {"execution","source_record","submission","queue"})
        if (command.at(key) != entry.at(key)) fail();
    Json available=Json::array();
    for (const auto &range : entry.at("ranges"))
        if (range.at("available_from_replayed_queries") == true) available.push_back(range);
    const auto &events=command.at("event_ids");
    if (available.empty() || !operations.is_array() || operations.size() != available.size() ||
        events.size() != operations.size()) fail();
    uint64_t previous{};
    for (size_t i=0;i<operations.size();++i) {
        const auto &actual=operations[i], &expected=available[i];
        const auto event=number(actual.at("eventId"));
        if (!event || event > UINT32_MAX || event <= previous || event != number(events[i]) ||
            actual.at("kind") != "buffer_range" || actual.at("resource_id") != resourceId ||
            actual.at("role") != "destination") fail();
        previous=event;
        for (const auto &[a,b] : {std::pair{"offset","offset"}, {"byte_length","bytes"},
                                 {"query_start","query_start"}, {"query_count","query_count"}})
            if (actual.at(a) != expected.at(b)) fail();
        if (i && (actual.at("query_heap") != operations[0].at("query_heap") ||
                  actual.at("query_type") != operations[0].at("query_type") ||
                  actual.at("query_result_stride") != operations[0].at("query_result_stride"))) fail();
    }
}
int64_t indirectActionOrdinal(const Json &p) {
    if (!p.contains("indirect_action") && !p.contains("indirect_action_count")) return -1;
    if (p.value("name", "") != "ID3D12GraphicsCommandListExecuteIndirect" ||
        !p.contains("indirect_action") || !p.contains("indirect_action_count"))
        throw std::runtime_error("Incomplete indirect action provenance");
    const auto ordinal = number(p.at("indirect_action")), count = number(p.at("indirect_action_count"));
    if (ordinal >= count) throw std::runtime_error("Indirect action ordinal exceeds its expansion");
    if (p.contains("event_ids")) {
        const auto &events = p.at("event_ids");
        if (!events.is_array() || events.size() != count)
            throw std::runtime_error("Indirect action count differs from its event mapping");
        const auto event = p.value("eventId", p.value("eid", Json()));
        if (!event.is_null() && number(event) != number(events.at(ordinal)))
            throw std::runtime_error("Indirect action ordinal identifies a different replay event");
    }
    return ordinal;
}
} // namespace flora
