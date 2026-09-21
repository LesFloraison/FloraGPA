#include "RdcEvents.h"
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
constexpr uint32_t clear = 1, draw = 2, dispatch = 4, push = 0x40, copy = 0x400, resolve = 0x800,
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
Json rdcProvenance(uint32_t event, const Json &reverse) {
    const auto found = reverse.find(std::to_string(event));
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
} // namespace flora
