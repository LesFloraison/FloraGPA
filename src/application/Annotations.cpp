#include "Annotations.h"
#include "ApiCommands.h"
#include "ContextInspector.h"
#include <QByteArray>
#include <QDir>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <map>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
bool annotation(uint16_t type) { return type >= 0x3278 && type <= 0x327e; }
bool marker(uint16_t type) { return type >= 0x327b && type <= 0x327d; }
using Owner = std::optional<Id>;
Owner ownerOf(const Json &command) {
    return command.contains("object") ? Owner(command.at("object").get<Id>()) : std::nullopt;
}
Json ownerJson(Owner owner) { return owner ? Json(*owner) : Json(nullptr); }
bool known(Owner owner) { return owner && *owner; }
Json field(const Json &command, const char *name) {
    for (const auto &f : command.at("fields"))
        if (f.at("name") == name)
            return f.at("value");
    return nullptr;
}
Json nodeBase(const Json &command, Owner owner, const Json &parent) {
    const auto &data = command.at("annotation");
    Json node{{"id", command.at("id")},
              {"object", ownerJson(owner)},
              {"parent", parent},
              {"kind", data.at("kind")},
              {"display_name", data.value("display_name", "EndEvent")},
              {"end", nullptr}};
    for (auto it = data.begin(); it != data.end(); ++it)
        if (it.key() != "kind" && it.key() != "display_name")
            node[it.key()] = it.value();
    return node;
}
} // namespace
Json inspectAnnotations(const Frame &frame) {
    const auto commands = inspectCommands(frame);
    Json nodes = Json::array(), issues = Json::array();
    std::vector<Id> commandIds;
    std::map<Owner, std::vector<size_t>> stacks;
    std::set<Owner> owners;
    size_t records = 0;
    auto issue = [&](Id id, const char *reason) { issues.push_back({{"event", id}, {"reason", reason}}); };
    auto abandon = [&](std::vector<size_t> &stack, const char *status, Id id) {
        for (auto index : stack) {
            nodes[index]["status"] = status;
            nodes[index]["interrupted_at"] = id;
        }
        stack.clear();
    };
    auto abandonAll = [&](Id id) {
        for (auto &[owner, stack] : stacks)
            abandon(stack, "interrupted_by_unknown_owner", id);
    };
    for (const auto &c : commands)
        commandIds.push_back(c.at("id").get<Id>());
    std::sort(commandIds.begin(), commandIds.end());
    for (const auto &c : commands) {
        const auto type = c.at("type").get<uint16_t>();
        if (!annotation(type))
            continue;
        ++records;
        const auto owner = ownerOf(c);
        const auto id = c.at("id").get<Id>();
        auto &stack = stacks[owner];
        owners.insert(owner);
        if (c.at("status") != "decoded") {
            abandon(stack, "interrupted_by_invalid_record", id);
            if (!known(owner))
                abandonAll(id);
            nodes.push_back({{"id", id},
                             {"object", ownerJson(owner)},
                             {"parent", nullptr},
                             {"kind", "invalid"},
                             {"display_name", c.at("name")},
                             {"status", c.at("status")},
                             {"end", nullptr},
                             {"error", c.value("error", Json(nullptr))}});
            issue(id, "Annotation wire record is not fully decoded");
            continue;
        }
        const auto &data = c.at("annotation");
        if (!known(owner) && type == 0x327a && data.at("ref_count") == 0) {
            abandonAll(id);
            issue(id, "Release to zero with unidentified annotation object ends all potentially affected "
                      "nesting streams");
            continue;
        }
        if (!known(owner) && marker(type)) {
            abandonAll(id);
            auto node = nodeBase(c, owner, nullptr);
            node["status"] = "unresolved_object";
            nodes.push_back(std::move(node));
            issue(id, "Zero annotation object reference cannot identify a nesting stream");
            continue;
        }
        if (type == 0x327a && data.at("ref_count") == 0)
            abandon(stack, "object_released_before_end", id);
        if (!marker(type))
            continue;
        auto node = nodeBase(c, owner, stack.empty() ? Json(nullptr) : nodes[stack.back()]["id"]);
        node["status"] = "marker";
        node["observed_depth"] = stack.size();
        const auto index = nodes.size();
        nodes.push_back(std::move(node));
        if (type == 0x327b) {
            nodes[index]["status"] = "open_at_capture_end";
            stack.push_back(index);
            const auto level = data.at("return_level").get<int64_t>();
            if (level >= 0 && uint64_t(level) != nodes[index]["observed_depth"].get<uint64_t>()) {
                nodes[index]["depth_differs_from_observed_calls"] = true;
                issue(id, "Returned nesting level differs from the visible call stack; capture prefix or "
                          "calls may be missing");
            }
        } else if (type == 0x327c) {
            nodes[index]["status"] = "unmatched_end";
            if (stack.empty()) {
                issue(id, "No visible BeginEvent for this annotation object");
                continue;
            }
            auto &begin = nodes[stack.back()];
            const auto first = begin.at("return_level").get<int64_t>(),
                       last = data.at("return_level").get<int64_t>();
            if (first >= 0 && last >= 0 && first != last) {
                nodes[index]["status"] = "depth_mismatch";
                abandon(stack, "interrupted_by_depth_mismatch", id);
                issue(id, "EndEvent level does not match the visible BeginEvent; no pair fabricated");
            } else {
                stack.pop_back();
                begin["end"] = id;
                begin["status"] = "closed";
                begin["end_return_level"] = last;
                begin["interval_command_count"] =
                    std::upper_bound(commandIds.begin(), commandIds.end(), id) -
                    std::lower_bound(commandIds.begin(), commandIds.end(), begin.at("id").get<Id>());
                nodes[index]["parent"] = begin.at("id");
                nodes[index]["status"] = "matched_end";
                nodes[index]["begin"] = begin.at("id");
            }
        }
    }
    struct Segment {
        Id start{};
        std::optional<Id> end;
        std::map<Id, Json> contexts;
        std::vector<Id> proofs;
        std::vector<size_t> nodes;
        bool conflict{};
    };
    std::vector<Segment> segments;
    std::map<Id, size_t> active, nodeIndices;
    Json links = Json::array();
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto &node = nodes[i];
        nodeIndices[node.at("id").get<Id>()] = i;
        for (auto key : {"context_ids", "context_proof_events", "draws", "unmatched_draw_ids"})
            node[key] = Json::array();
        node["context_association"] = "unresolved";
        node["membership"] = "not_a_closed_group";
    }
    auto close = [&](Id owner, Id event) {
        auto it = active.find(owner);
        if (it != active.end()) {
            segments[it->second].end = event;
            active.erase(it);
        }
    };
    for (const auto &c : commands) {
        const auto type = c.at("type").get<uint16_t>();
        if (!annotation(type))
            continue;
        const auto owner = ownerOf(c);
        const auto id = c.at("id").get<Id>();
        if (!known(owner)) {
            if (c.at("status") != "decoded" || (type == 0x327a && c.at("annotation").at("ref_count") == 0)) {
                for (const auto &[key, index] : active)
                    segments[index].end = id;
                active.clear();
            }
            continue;
        }
        if (c.at("status") != "decoded") {
            close(*owner, id);
            continue;
        }
        if (!active.contains(*owner)) {
            active[*owner] = segments.size();
            segments.push_back(Segment{.start = id});
        }
        auto &segment = segments[active.at(*owner)];
        if (nodeIndices.contains(id))
            segment.nodes.push_back(nodeIndices.at(id));
        if (type == 0x3278) {
            const auto bytes =
                QByteArray::fromHex(QByteArray::fromStdString(field(c, "interface_guid").get<std::string>()));
            Reader raw(Bytes(reinterpret_cast<const uint8_t *>(bytes.data()), size_t(bytes.size())));
            const auto d1 = raw.read<uint32_t>();
            const auto d2 = raw.read<uint16_t>(), d3 = raw.read<uint16_t>();
            const auto guid = QUuid(d1, d2, d3, uint8_t(bytes[8]), uint8_t(bytes[9]), uint8_t(bytes[10]),
                                    uint8_t(bytes[11]), uint8_t(bytes[12]), uint8_t(bytes[13]),
                                    uint8_t(bytes[14]), uint8_t(bytes[15]))
                                  .toString(QUuid::WithoutBraces)
                                  .toStdString();
            const auto hr = field(c, "hresult").get<int64_t>();
            const auto returned = field(c, "returned_object").get<Id>();
            Json link{{"event", id},
                      {"object", *owner},
                      {"guid", guid},
                      {"hresult", hr},
                      {"returned_object", returned},
                      {"status", "unsupported_interface"}};
            if (guid == "c0bfa96c-e089-44fb-8eaf-26f8796190da" ||
                guid == "bb2c6faa-b5fb-4082-8e6b-388b8cfa90e1") {
                link["interface"] = guid.front() == 'c' ? "ID3D11DeviceContext" : "ID3D11DeviceContext1";
                if (hr != 0)
                    link["status"] = hr < 0 ? "query_failed" : "unexpected_hresult";
                else if (!returned)
                    link["status"] = "missing_returned_identity";
                else {
                    try {
                        auto context = contextJson(describeContext(frame, returned, false));
                        link["status"] = "resolved";
                        link["context"] = context;
                        for (const auto &[key, previous] : segment.contexts)
                            if (previous.at("context_type") != context.at("context_type") ||
                                (previous.at("device") != 0 && context.at("device") != 0 &&
                                 previous.at("device") != context.at("device")))
                                segment.conflict = true;
                        segment.contexts[returned] = std::move(context);
                        segment.proofs.push_back(id);
                    } catch (const std::exception &error) {
                        link["status"] = "invalid_context_resource";
                        link["reason"] = error.what();
                    }
                }
            }
            links.push_back(std::move(link));
        }
        if (type == 0x327a && c.at("annotation").at("ref_count") == 0)
            close(*owner, id);
    }
    for (const auto &segment : segments) {
        std::vector<Id> ids;
        if (!segment.conflict)
            for (const auto &[id, context] : segment.contexts)
                ids.push_back(id);
        if (segment.conflict)
            issue(segment.start, "Annotation QueryInterface identities disagree on context kind or owning "
                                 "device; association withheld");
        for (auto index : segment.nodes) {
            auto &node = nodes[index];
            node["context_ids"] = ids;
            node["context_proof_events"] = segment.proofs;
            node["context_association"] = segment.conflict ? "conflicting_context_identity"
                                          : ids.empty()    ? "unresolved"
                                                           : "explicit_query_interface";
            node["identity_segment"] = {{"start", segment.start},
                                        {"end", segment.end ? Json(*segment.end) : Json(nullptr)}};
            if (node["kind"] != "BeginEvent" ||
                (node["status"] != "closed" && node["status"] != "open_at_capture_end"))
                continue;
            const auto begin = node["id"].get<Id>(),
                       end = node["end"].is_null() ? commandIds.back() : node["end"].get<Id>();
            if (segment.end && end >= *segment.end) {
                node["membership"] = "identity_boundary";
                continue;
            }
            if (ids.empty()) {
                node["membership"] = "unresolved_context";
                continue;
            }
            node["membership"] = node["end"].is_null() ? "open_capture_suffix_exact_context_ids"
                                                       : "exact_captured_context_ids";
            node["membership_range"] = {{"start", begin},
                                        {"end", end},
                                        {"inclusive", true},
                                        {"complete_group", !node["end"].is_null()}};
            for (const auto &c : commands) {
                const auto id = c.at("id").get<Id>();
                if (id < begin || id > end || !isDraw(c.at("type").get<uint16_t>()))
                    continue;
                const auto context = field(c, "context");
                if (c.at("status") == "decoded" && !context.is_null() &&
                    std::binary_search(ids.begin(), ids.end(), context.get<Id>()))
                    node["draws"].push_back({{"id", id}, {"name", c.at("name")}, {"context", context}});
                else
                    node["unmatched_draw_ids"].push_back(id);
            }
        }
    }
    Json objects = Json::array();
    for (auto owner : owners)
        objects.push_back(ownerJson(owner));
    const bool associated =
        std::any_of(nodes.begin(), nodes.end(), [](const auto &n) { return !n.at("context_ids").empty(); });
    const bool membership =
        std::any_of(nodes.begin(), nodes.end(), [](const auto &n) { return n.contains("membership_range"); });
    return {
        {"frame", QString::fromStdWString(frame.path().filename().wstring()).toStdString()},
        {"source", "captured_annotation_calls"},
        {"hierarchy", "observed_per_object_call_nesting"},
        {"record_count", records},
        {"objects", objects},
        {"nodes", nodes},
        {"issues", issues},
        {"context_association_restored", associated},
        {"draw_membership_restored", membership},
        {"context_links", links},
        {"context_association_scope", "explicit_query_interface_within_observed_object_lifetime"},
        {"draw_membership_scope", "observed_groups_exact_captured_context_ids"},
        {"limits",
         {"Annotation object IDs are not assumed to be context IDs.",
          "Range statistics use global captured command order; the separate draw association list requires "
          "explicit context identity proofs.",
          "Negative API return levels are retained; observed call nesting does not claim that the native "
          "profiler accepted the event.",
          "Missing capture-prefix and malformed records are not repaired with invented groups.",
          "Context associations require S_OK, a mapped context GUID and a valid captured context resource; "
          "matching numeric IDs alone are not evidence.",
          "COM identity proofs apply within the observed object lifetime, including earlier calls in the "
          "same uninterrupted segment; release-to-zero and malformed records end the segment.",
          "Group draws match only explicitly proven captured context IDs. A capture-end open group lists "
          "only the captured suffix, without inventing an EndEvent. Interrupted groups and other context "
          "aliases remain unresolved; unmatched draws are not claimed to belong to another native context.",
          "Deferred-context draw membership describes recorded calls, not command-list execution on the "
          "GPU."}}};
}
void exportAnnotations(const Json &report, const std::filesystem::path &directory) {
    const auto root = QString::fromStdWString(directory.wstring());
    if (!QDir().mkpath(root))
        throw std::runtime_error("Cannot create annotation export directory");
    auto save = [&](const char *name, const QByteArray &bytes) {
        QSaveFile file(root + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            throw std::runtime_error("Cannot save annotation export");
    };
    save("annotations.json", QByteArray::fromStdString(report.dump(2) + "\n"));
    QByteArray csv =
        QByteArray::fromHex("efbbbf") + "id,annotation_object,parent_begin,kind,name,status,end_id,context_"
                                        "ids,context_proof_events,membership,draw_ids,unmatched_draw_ids\r\n";
    for (const auto &node : report.at("nodes")) {
        Json draws = Json::array();
        for (const auto &draw : node.at("draws"))
            draws.push_back(draw.at("id"));
        std::vector<Json> values;
        for (auto key : {"id", "object", "parent", "kind", "display_name", "status", "end", "context_ids",
                         "context_proof_events", "membership"})
            values.push_back(node.at(key));
        values.push_back(draws);
        values.push_back(node.at("unmatched_draw_ids"));
        for (size_t i = 0; i < values.size(); ++i) {
            const auto &value = values[i];
            auto text = QByteArray::fromStdString(value.is_null()     ? ""
                                                  : value.is_string() ? value.get<std::string>()
                                                                      : value.dump());
            text.replace("\"", "\"\"");
            if (i)
                csv += ',';
            csv += '"' + text + '"';
        }
        csv += "\r\n";
    }
    save("annotations.csv", csv);
}
} // namespace flora
