#include "InitializationGraph.h"
#include "ApiCommands.h"
#include "InitializationReferences.h"
#include <set>

namespace flora {
using Json = nlohmann::json;
namespace {
struct References {
    Json items = Json::array(), sequence = Json::array();
    std::set<uint32_t> dependencies;
    void add(const std::string &field, Id value, bool omitZero = true) {
        auto original = uint32_t(value);
        if (omitZero && !original)
            return;
        items.push_back({{"field", field}, {"captured_id", value}, {"original_id", original}});
        sequence.push_back(original);
        dependencies.insert(original);
    }
    template <size_t N> void array(const std::string &name, const std::array<Id, N> &values) {
        for (size_t i = 0; i < N; ++i)
            add(name + "[" + std::to_string(i) + "]", values[i]);
    }
    void finish(Json &out, const char *collector) const {
        out.update({{"status", "recovered"},
                    {"collector_rva", collector},
                    {"references", items},
                    {"collector_sequence", sequence},
                    {"dependency_set", dependencies}});
    }
};
void stateReferences(const State &state, References &refs) {
    constexpr std::array<size_t, 6> order{0, 3, 4, 1, 2, 5}; // VS GS PS HS DS CS
    constexpr std::array<const char *, 6> names{"vs", "hs", "ds", "gs", "ps", "cs"};
    auto field = [&](size_t stage, const char *kind, size_t i) {
        return std::string(names[stage]) + "." + kind + "[" + std::to_string(i) + "]";
    };
    refs.add("layout", state.layout);
    refs.array("vb", state.vb);
    for (size_t i = 0; i < 14; ++i)
        for (auto stage : order)
            refs.add(field(stage, "cb", i), state.stages[stage].cb[i]);
    for (size_t i = 0; i < 16; ++i)
        for (auto stage : order)
            refs.add(field(stage, "samplers", i), state.stages[stage].samplers[i]);
    for (size_t i = 0; i < 128; ++i)
        for (auto stage : order)
            refs.add(field(stage, "srv", i), state.stages[stage].srv[i]);
    for (auto stage : order)
        refs.add(std::string(names[stage]) + ".shader", state.stages[stage].shader);
    for (auto stage : order) {
        // Original 0x63d80 omits CS classes and the index buffer entirely.
        if (stage == 5)
            continue;
        const auto &s = state.stages[stage];
        if (s.classCount > s.classes.size())
            throw std::runtime_error("State class count exceeds saved array");
        for (size_t i = 0; i < s.classCount; ++i)
            refs.add(field(stage, "classes", i), s.classes[i]);
    }
    refs.array("cs_uav", state.csUav);
    refs.array("cs_extended", state.csExtended);
    refs.array("so", state.so);
    refs.add("scissors", state.scissors);
    refs.add("rasterizer", state.rasterizer);
    refs.add("viewports", state.viewports);
    refs.add("blend", state.blend);
    refs.add("depth_state", state.depthState);
    refs.add("dsv", state.dsv);
    refs.array("rtv", state.rtv);
    refs.array("om_extended", state.omExtended);
    refs.add("predicate", state.predicate);
}
struct ResourceRule {
    size_t size;
    const char *collector;
    bool ownerAlways;
};
const std::map<uint16_t, ResourceRule> resources{
    {0x38, {88, "0x5c400", false}}, {0x81, {28, "0x5c460", false}},  {0x82, {24, "0x5be40", false}},
    {0x83, {48, "0x5b960", false}}, {0x84, {56, "0x5c180", false}},  {0x85, {68, "0x5c210", false}},
    {0x86, {60, "0x5c300", false}}, {0x87, {68, "0x5bc80", true}},   {0x88, {68, "0x5c0c0", false}},
    {0x8b, {68, "0x5bbc0", true}},  {0x8c, {48, "0x5c110", false}},  {0x8d, {44, "0x5c050", true}},
    {0x8e, {48, "0x5bc10", true}},  {0x8f, {44, "0x5c390", false}},  {0x90, {56, "0x5bb20", false}},
    {0x92, {56, "0x5bb20", false}}, {0x93, {56, "0x5bb20", false}},  {0x94, {56, "0x5bb20", false}},
    {0x95, {56, "0x5bb20", false}}, {0x10d, {344, "0x5b8c0", true}}, {0x10f, {64, "0x5bfb0", true}},
    {0x127, {24, "0x5c410", true}}, {0x9a, {16, "0x5bad0", true}},
};
const std::map<uint16_t, const char *> dataCollectors{
    {1, "0x16870"},     {0x81, "0xb65f0"},  {0x84, "0x5bdd0"},  {0x86, "0xb6a80"},  {0x87, "0xba8e0"},
    {0x100, "0x16970"}, {0x101, "0x166c0"}, {0x102, "0x16870"}, {0x103, "0x16b20"}, {0x104, "0x45590"},
};
void dataReferences(uint16_t type, Reader &r, References &refs) {
    auto blob = [&] { r.skip(r.read<uint32_t>()); };
    if (type == 1 || type == 0x102)
        blob();
    else if (type == 0x101) {
        blob();
        blob();
    } else if (type == 0x100 || type == 0x81) {
        const auto a = r.read<uint32_t>(), b = r.read<uint32_t>();
        r.skip(a);
        r.skip(b);
        if (type == 0x81) {
            blob();
            r.skip(uint64_t(r.read<uint32_t>()) * 8);
        }
    } else if (type == 0x103) {
        r.skip(24);
        blob();
    } else if (type == 0x104) {
        refs.add("owner", r.read<Id>(), false);
        r.skip(4);
    } else if (type == 0x86 || type == 0x87)
        r.skip(uint64_t(r.read<uint32_t>()) * (type == 0x86 ? 16 : 24));
    else if (type == 0x84) {
        const auto count = r.read<uint32_t>();
        if (count > r.remaining() / 32)
            throw std::runtime_error("Input layout array exceeds saved bytes");
        for (uint32_t i = 0; i < count; ++i) {
            refs.add("semantics[" + std::to_string(i) + "]", r.read<Id>(), false);
            r.skip(24);
        }
        blob();
    }
    r.end();
}
} // namespace

Json inspectInitializationNode(const Frame &frame, Id id) {
    const auto &entry = frame.entry(id);
    const auto kind = entry.category == 3   ? "state"
                      : entry.category == 5 ? "resource"
                      : entry.category == 9 ? "data"
                                            : "unknown";
    Json out{{"id", id},
             {"kind", kind},
             {"type", entry.type},
             {"status", "unrecovered"},
             {"scope", "original_initial_dependency_collection"},
             {"resource_objects_validated", false}};
    References refs;
    try {
        if (frame.hasEditedViews()) {
            out["reason"] = "Edited version collectors are not recovered";
            return out;
        }
        if (entry.category == 3 && entry.type == 3) {
            stateReferences(frame.state(id), refs);
            refs.finish(out, "0x63d80");
        } else if (entry.category == 9 && dataCollectors.contains(entry.type)) {
            Reader r(frame.payload(id));
            dataReferences(entry.type, r, refs);
            refs.finish(out, dataCollectors.at(entry.type));
        } else if (entry.category == 5 && resources.contains(entry.type)) {
            const auto &rule = resources.at(entry.type);
            auto bytes = frame.payload(id);
            if (bytes.size() != rule.size)
                throw std::runtime_error("Unexpected resource record length");
            auto q = [&](size_t offset) {
                Reader r(bytes.subspan(offset));
                return r.read<Id>();
            };
            auto u = [&](size_t offset) {
                Reader r(bytes.subspan(offset));
                return r.read<uint32_t>();
            };
            const auto t = entry.type;
            if (t != 0x38 && t != 0x81 && t != 0x82)
                refs.add(t == 0x9a ? "parent_context" : "owner", q(8), !rule.ownerAlways);
            if (t >= 0x83 && t <= 0x87) {
                refs.add("data", q(bytes.size() - 8));
                if (t == 0x85 && (u(52) & 0x80000000u) && u(44) == 0)
                    refs.add("original", q(0), false);
            } else if (t == 0x82) {
                if (!uint32_t(q(16)))
                    throw std::runtime_error("Original input layout collector rejects zero data identity");
                refs.add("data", q(16));
            } else if (t >= 0x8c && t <= 0x8f)
                refs.add("resource", q(16), false);
            else if (t >= 0x90 && t <= 0x95) {
                refs.add("data", q(48), false);
                refs.add("linkage", q(32));
            }
            refs.finish(out, rule.collector);
        } else
            out["reason"] = "No verified node collector for this category and wire type";
    } catch (const std::exception &error) {
        out.update({{"status", "invalid_record"}, {"reason", error.what()}});
    }
    return out;
}

Json inspectInitializationGraph(const Frame &frame) {
    const auto cache = initialFileCache(frame);
    Json out{{"schema", "FloraGPA initial dependency graph 1"},
             {"profile", "GPA_2025_R1_dx11_player_39061ff3"},
             {"cache", cache.report()},
             {"nodes", Json::array()},
             {"issues", Json::array()},
             {"dependency_graph_complete", false},
             {"initialization_schedule_available", false},
             {"execution_supported", false}};
    if (!cache.complete)
        return out;
    std::map<Id, Json> api;
    for (auto &command : inspectCommands(frame)) {
        const auto id = command["id"].get<Id>();
        api.emplace(id, std::move(command));
    }
    bool complete = true;
    for (const auto &[kind, ids] : cache.descriptors) {
        for (auto id : ids) {
            Json node;
            if (kind == 3) {
                node = api.at(id).at("original_initialization");
                node.update({{"id", id}, {"kind", "erg"}, {"type", frame.entry(id).type}});
            } else
                node = inspectInitializationNode(frame, id);
            if (node["status"] != "recovered")
                complete = false;
            else
                for (const auto &dependency : node["dependency_set"]) {
                    if (!cache.contains(dependency.get<uint32_t>())) {
                        complete = false;
                        out["issues"].push_back(
                            {{"id", id},
                             {"dependency", dependency},
                             {"reason", "Dependency has no registered initial descriptor"}});
                    }
                }
            out["nodes"].push_back(std::move(node));
        }
    }
    out["dependency_graph_complete"] = complete;
    return out;
}
} // namespace flora
