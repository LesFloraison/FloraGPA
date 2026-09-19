#include "CommandState.h"
#include "ApiCommands.h"
#include "core/Contexts.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace flora {
namespace {
using Json = nlohmann::json;
using Values = std::vector<std::pair<std::string, Json>>;
constexpr const char *stages[]{"vs", "hs", "ds", "gs", "ps", "cs"};
Json finite(float value) {
    if (std::isfinite(value))
        return value;
    return std::isnan(value) ? "nan" : value > 0 ? "inf" : "-inf";
}
Values snapshot(const Frame *frame, const State &s, bool compute = false) {
    Values out{{"ib", s.ib},
               {"ib_format", s.ibFormat},
               {"ib_offset", s.ibOffset},
               {"input_layout", s.layout},
               {"topology", s.topology},
               {"rasterizer", s.rasterizer},
               {"blend", s.blend},
               {"sample_mask", s.sampleMask},
               {"depth_state", s.depthState},
               {"stencil_ref", s.stencilRef},
               {"dsv", s.dsv},
               {"predicate", s.predicate},
               {"predicate_value", s.predicateValue}};
    auto array = [&](const std::string &key, const auto &values) {
        for (size_t i = 0; i < values.size(); ++i)
            out.emplace_back(key + '.' + std::to_string(i), values[i]);
    };
    array("vb", s.vb);
    array("strides", s.strides);
    array("offsets", s.offsets);
    for (size_t i = 0; i < 4; ++i)
        out.emplace_back("blend_factor." + std::to_string(i), finite(s.blendFactor[i]));
    for (size_t i = 0; i < 6; ++i) {
        const auto &stage = s.stages[i];
        const std::string prefix = stages[i];
        if (!compute || i == 5)
            out.emplace_back(prefix + ".shader", stage.shader);
        auto classes = Json::array();
        for (size_t n = 0; n < std::min<size_t>(stage.classCount, stage.classes.size()); ++n)
            classes.push_back(stage.classes[n]);
        out.emplace_back(prefix + ".classes", classes);
        array(prefix + ".cb", stage.cb);
        array(prefix + ".srv", stage.srv);
        array(prefix + ".samplers", stage.samplers);
    }
    for (size_t i = 0; i < 8; ++i)
        out.emplace_back("rtv." + std::to_string(i), i < std::min(s.rtCount, s.omStart) ? s.rtv[i] : 0);
    for (size_t i = 0; i < 64; ++i)
        out.emplace_back("om.uav." + std::to_string(i),
                         i >= s.omStart && i < s.rtCount ? (i < 8 ? s.rtv[i] : s.omExtended[i - 8]) : 0);
    for (size_t i = 0; i < 64; ++i)
        out.emplace_back("cs.uav." + std::to_string(i), i < 8 ? s.csUav[i] : s.csExtended[i - 8]);
    for (size_t i = 0; i < 4; ++i)
        out.emplace_back("so.targets." + std::to_string(i), i < s.soCount ? s.so[i] : 0);
    for (auto [name, id] : {std::pair{"viewports", s.viewports}, std::pair{"scissors", s.scissors}}) {
        auto rows = Json::array();
        if (id) {
            if (!frame)
                throw std::runtime_error("Missing capture for state array");
            Reader r(frame->payload(id));
            r.skip(4);
            while (r.remaining()) {
                auto row = Json::array();
                if (id == s.viewports && std::string(name) == "viewports")
                    for (int i = 0; i < 6; ++i)
                        row.push_back(finite(r.read<float>()));
                else
                    for (int i = 0; i < 4; ++i)
                        row.push_back(r.read<int32_t>());
                rows.push_back(row);
            }
        }
        out.emplace_back(name, rows);
    }
    return out;
}
const Values &defaults() {
    static const auto values = [] {
        State state{};
        state.sampleMask = UINT32_MAX;
        state.blendFactor.fill(1);
        auto out = snapshot(nullptr, state);
        for (auto stage : stages)
            for (int i = 0; i < 14; ++i)
                out.emplace_back(std::string(stage) + ".cb_range." + std::to_string(i),
                                 Json::array({0, nullptr}));
        for (int i = 0; i < 4; ++i)
            out.emplace_back("so.offsets." + std::to_string(i), 0);
        return out;
    }();
    return values;
}
bool outputField(const std::string &k) {
    return k.starts_with("rtv.") || k.starts_with("om.uav.") || k.starts_with("cs.uav.") ||
           k.starts_with("so.targets.") || k == "dsv";
}
bool inputField(const std::string &k) {
    return k.find(".srv.") != k.npos || k.starts_with("vb.") || k == "ib";
}
struct Span {
    Id resource{};
    std::optional<std::set<std::pair<uint32_t, uint32_t>>> subresources;
    bool ambiguous{};
};
class ContextState {
    const Frame &frame_;
    std::map<Id, Span> spans_;
    std::vector<std::string> keys_;
    Json values_ = Json::object(), sources_ = Json::object();
    void put(const std::string &key, const Json &value, Id event, const std::string &kind = "command",
             const std::string &reason = "") {
        if (!values_.contains(key))
            keys_.push_back(key);
        values_[key] = value;
        sources_[key] = {{"kind", kind}, {"event", event}};
        if (!reason.empty())
            sources_[key]["reason"] = reason;
    }
    void invalidate(Id event, const std::string &reason, const std::vector<std::string> &keys) {
        for (const auto &key : keys)
            put(key, nullptr, event, "unknown", reason);
    }
    std::optional<Span> span(Id id) {
        if (auto cached = spans_.find(id); cached != spans_.end())
            return cached->second;
        auto found = frame_.entries().find(id);
        if (found == frame_.entries().end() || found->second.category != 5)
            return {};
        auto type = found->second.type;
        Span result;
        if (type == 0x83)
            result.resource = id;
        else if (type >= 0x8c && type <= 0x8f) {
            Reader r(frame_.payload(id));
            r.skip(16);
            auto owner = r.read<Id>();
            std::vector<uint32_t> fields;
            while (r.remaining())
                fields.push_back(r.read<uint32_t>());
            auto dim = fields.at(1);
            auto resource = frame_.resource(owner);
            result.resource = owner;
            if (resource.type != 0x83) {
                auto info = textureInfo(resource);
                uint64_t mip = 0, layer = 0, mips = 1, layers = 1;
                result.ambiguous = (type == 0x8e && (fields.at(2) & 3)) || info.dimension == 4;
                if (type == 0x8c) {
                    if (dim < 2 || dim > 10)
                        return {};
                    if (dim == 2 || dim == 3 || dim == 4 || dim == 5 || dim == 8 || dim == 9 || dim == 10) {
                        mip = fields.at(2);
                        mips = fields.at(3);
                        if (mips == UINT32_MAX)
                            mips = mip <= info.mips ? info.mips - mip : 0;
                    }
                    if (dim == 3 || dim == 5) {
                        layer = fields.at(4);
                        layers = fields.at(5);
                    }
                    if (dim == 7) {
                        layer = fields.at(2);
                        layers = fields.at(3);
                    }
                    if (dim == 9)
                        layers = 6;
                    if (dim == 10) {
                        layer = fields.at(4);
                        layers = uint64_t(fields.at(5)) * 6;
                    }
                } else if (type == 0x8e) {
                    if (dim < 1 || dim > 6)
                        return {};
                    if (dim <= 4)
                        mip = fields.at(3);
                    if (dim == 2 || dim == 4) {
                        layer = fields.at(4);
                        layers = fields.at(5);
                    }
                    if (dim == 6) {
                        layer = fields.at(3);
                        layers = fields.at(4);
                    }
                } else {
                    if (dim < 2 || dim > 8)
                        return {};
                    if (dim == 2 || dim == 3 || dim == 4 || dim == 5 || dim == 8)
                        mip = fields.at(2);
                    if (dim == 3 || dim == 5) {
                        layer = fields.at(3);
                        layers = fields.at(4);
                    }
                    if (dim == 7) {
                        layer = fields.at(2);
                        layers = fields.at(3);
                    }
                }
                if (!mips || !layers || mip + mips > info.mips || layer + layers > info.layers ||
                    mips * layers > 65536)
                    return {};
                result.subresources.emplace();
                for (uint64_t m = mip; m < mip + mips; ++m)
                    for (uint64_t l = layer; l < layer + layers; ++l)
                        result.subresources->emplace(uint32_t(m), uint32_t(l));
            }
        } else
            return {};
        spans_.emplace(id, result);
        return result;
    }
    std::optional<bool> overlap(const Json &a, const Json &b) {
        if (a == 0 || b == 0)
            return false;
        if (a.is_null() || b.is_null())
            return {};
        auto left = span(a.get<Id>()), right = span(b.get<Id>());
        if (!left || !right)
            return {};
        if (left->resource != right->resource)
            return false;
        if (left->subresources && right->subresources) {
            bool intersects = false;
            for (auto sub : *left->subresources)
                if (right->subresources->contains(sub)) {
                    intersects = true;
                    break;
                }
            if (!intersects)
                return false;
        }
        if (left->ambiguous || right->ambiguous)
            return {};
        return true;
    }
    void input(const std::string &key, const Json &id, Id event) {
        bool conflict = false, unknown = false;
        for (const auto &k : keys_)
            if (outputField(k)) {
                auto result = overlap(id, values_[k]);
                unknown |= !result;
                conflict |= result.value_or(false);
            }
        if (conflict)
            put(key, 0, event, "hazard_null");
        else if (unknown)
            invalidate(event, "Output overlap cannot be determined", {key});
        else
            put(key, id, event);
    }
    void output(const Json &changes, Id event) {
        std::vector<Json> ids;
        for (auto it = changes.begin(); it != changes.end(); ++it)
            if (!it.value().is_null() && it.value() != 0)
                ids.push_back(it.value());
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j)
                if (overlap(ids[i], ids[j]) != std::optional<bool>(false))
                    throw std::runtime_error("Overlapping or unresolved simultaneous output views");
        for (const auto &key : keys_) {
            if (changes.contains(key) || (!outputField(key) && !inputField(key)))
                continue;
            bool conflict = false, unknown = false;
            for (const auto &id : ids) {
                auto r = overlap(values_[key], id);
                unknown |= !r;
                conflict |= r.value_or(false);
            }
            if (conflict)
                put(key, 0, event, "hazard_null");
            else if (unknown)
                invalidate(event, "Output binding may invalidate this resource slot", {key});
        }
        for (auto it = changes.begin(); it != changes.end(); ++it)
            put(it.key(), it.value(), event);
    }

  public:
    Json notes = Json::array();
    explicit ContextState(const Frame &frame) : frame_(frame) {
        for (const auto &[key, value] : defaults()) {
            keys_.push_back(key);
            values_[key] = nullptr;
            sources_[key] = {{"kind", "unknown_initial"}, {"event", nullptr}};
        }
    }
    std::pair<size_t, Json> anchor(const Json &command) {
        size_t checked = 0;
        auto differences = Json::array();
        auto state = frame_.state(command.at("state_id").get<Id>());
        for (const auto &[key, value] :
             snapshot(&frame_, state, command.at("name").get<std::string>().starts_with("Dispatch"))) {
            const auto old = values_[key];
            if (!old.is_null()) {
                ++checked;
                if (old != value)
                    differences.push_back({{"field", key}, {"predicted", old}, {"captured", value}});
            }
            if (key.find(".cb.") != key.npos && value != old) {
                auto range = key;
                range.replace(range.find(".cb."), 4, ".cb_range.");
                invalidate(command.at("id"), "CB binding changed without an observed range setter", {range});
            }
            put(key, value, command.at("id"), "snapshot");
        }
        return {checked, differences};
    }
    void apply(const Json &c) {
        Id eid = c.at("id");
        std::string n = c.at("name");
        Json d = Json::object();
        for (const auto &field : c.at("fields"))
            d[field.at("name").get<std::string>()] = field.at("value");
        auto arr = [&](const std::string &name, uint64_t count, const Json &fallback = 0) {
            std::vector<Json> out;
            for (uint64_t i = 0; i < count; ++i)
                out.push_back(d.value(name + '[' + std::to_string(i) + ']', fallback));
            return out;
        };
        auto bounds = [](uint64_t start, uint64_t count, uint64_t limit) {
            if (start > limit || count > limit - start)
                throw std::runtime_error("Binding range outside API bounds");
        };
        auto bind = [&](const std::string &prefix, const std::string &name, uint64_t start, uint64_t count,
                        uint64_t limit) {
            bounds(start, count, limit);
            auto values = arr(name, count);
            for (size_t i = 0; i < values.size(); ++i) {
                auto key = prefix + '.' + std::to_string(start + i);
                if (inputField(key))
                    input(key, values[i], eid);
                else
                    put(key, values[i], eid);
            }
        };
        auto get = [&](const char *key) -> const Json & { return d.at(key); };
        try {
            if (c.at("status") != "decoded")
                throw std::runtime_error("Command layout is not fully decoded");
            if (n == "ClearState")
                for (const auto &[key, value] : defaults())
                    put(key, value, eid, "clear_state");
            else if (n.find("SetConstantBuffers") != n.npos || n.ends_with("SetShaderResources") ||
                     n.ends_with("SetSamplers")) {
                std::string key = n.find("ConstantBuffers") != n.npos   ? "cb"
                                  : n.find("ShaderResources") != n.npos ? "srv"
                                                                        : "samplers";
                auto stage = n.substr(0, 2);
                std::transform(stage.begin(), stage.end(), stage.begin(),
                               [](unsigned char ch) { return char(std::tolower(ch)); });
                uint64_t start = get("start_slot"), count = get("count"),
                         limit = key == "cb"    ? 14
                                 : key == "srv" ? 128
                                                : 16;
                // Present arrays already passed the wire decoder's 65536-element bound.
                // Preserve validation order when a CB1 window and binding range are both invalid.
                if (key == "cb" && n.ends_with('1')) {
                    if (get("first_constants_present") != get("constant_counts_present"))
                        throw std::runtime_error("CB1 offset and count must both be present or absent");
                    if (get("first_constants_present") != 0) {
                        auto first = arr("first_constants", count), sizes = arr("constant_counts", count);
                        for (size_t i = 0; i < first.size(); ++i)
                            if (first[i].get<uint64_t>() % 16 || sizes[i].get<uint64_t>() % 16 ||
                                sizes[i].get<uint64_t>() > 4096)
                                throw std::runtime_error("Invalid CB1 window granularity or count");
                    }
                }
                bind(stage + '.' + key, "bindings", start, count, limit);
                if (key == "cb")
                    for (uint64_t i = 0; i < count; ++i)
                        put(stage + ".cb_range." + std::to_string(start + i),
                            Json::array(
                                {d.value("first_constants[" + std::to_string(i) + ']', Json(0)),
                                 d.value("constant_counts[" + std::to_string(i) + ']', Json(nullptr))}),
                            eid);
            } else if (n.ends_with("SetShader")) {
                uint64_t count = get("class_count");
                bounds(0, count, 256);
                auto stage = n.substr(0, 2);
                std::transform(stage.begin(), stage.end(), stage.begin(),
                               [](unsigned char ch) { return char(std::tolower(ch)); });
                put(stage + ".shader", get("shader"), eid);
                put(stage + ".classes", arr("class_instances", count), eid);
            } else if (n == "IASetVertexBuffers") {
                for (auto [key, name] :
                     {std::pair{"vb", "buffers"}, {"strides", "strides"}, {"offsets", "offsets"}})
                    bind(key, name, get("start_slot"), get("count"), 32);
            } else if (n == "IASetIndexBuffer") {
                input("ib", get("buffer"), eid);
                put("ib_format", get("format"), eid);
                put("ib_offset", get("offset"), eid);
            } else if (n == "IASetInputLayout" || n == "IASetPrimitiveTopology" || n == "RSSetState") {
                auto key = n == "IASetInputLayout"         ? "input_layout"
                           : n == "IASetPrimitiveTopology" ? "topology"
                                                           : "rasterizer";
                put(key, get(key), eid);
            } else if (n == "RSSetViewports" || n == "RSSetScissorRects") {
                auto key = n == "RSSetViewports" ? "viewports" : "scissors";
                uint64_t count = get("count");
                bounds(0, count, 16);
                put(key, arr(key, count), eid);
            } else if (n == "OMSetBlendState") {
                put("blend", get("blend_state"), eid);
                put("sample_mask", get("sample_mask"), eid);
                auto v = arr("blend_factor", 4, 1.0);
                for (size_t i = 0; i < 4; ++i)
                    put("blend_factor." + std::to_string(i), v[i], eid);
            } else if (n == "OMSetDepthStencilState") {
                put("depth_state", get("depth_state"), eid);
                put("stencil_ref", get("stencil_ref"), eid);
            } else if (n == "SetPredication") {
                put("predicate", get("predicate"), eid);
                put("predicate_value", get("value"), eid);
            } else if (n == "CSSetUnorderedAccessViews") {
                uint64_t start = get("start_slot"), count = get("count");
                bounds(start, count, 64);
                Json changes = Json::object();
                auto values = arr("uavs", count);
                for (size_t i = 0; i < values.size(); ++i)
                    changes["cs.uav." + std::to_string(start + i)] = values[i];
                output(changes, eid);
            } else if (n == "SOSetTargets") {
                uint64_t count = get("count");
                bounds(0, count, 4);
                Json changes = Json::object();
                auto values = arr("buffers", 4);
                for (size_t i = 0; i < 4; ++i)
                    changes["so.targets." + std::to_string(i)] = values[i];
                output(changes, eid);
                auto offsets = arr("offsets", 4);
                for (size_t i = 0; i < 4; ++i)
                    put("so.offsets." + std::to_string(i),
                        i < count && get("offsets_present") == 0 ? Json(nullptr) : offsets[i], eid);
            } else if (n == "OMSetRenderTargets") {
                bounds(0, get("count"), 8);
                Json changes = Json::object();
                auto values = arr("rtvs", 8);
                for (size_t i = 0; i < 8; ++i)
                    changes["rtv." + std::to_string(i)] = values[i];
                changes["dsv"] = get("dsv");
                for (int i = 0; i < 64; ++i)
                    changes["om.uav." + std::to_string(i)] = 0;
                output(changes, eid);
            } else if (n == "OMSetRenderTargetsAndUnorderedAccessViews") {
                Json changes = Json::object();
                uint64_t rt = get("rtv_count"), uc = get("uav_count"), start = get("uav_start");
                if (rt != UINT32_MAX) {
                    bounds(0, rt, 8);
                    auto values = arr("rtvs", 8);
                    for (size_t i = 0; i < 8; ++i)
                        changes["rtv." + std::to_string(i)] = values[i];
                    changes["dsv"] = get("dsv");
                }
                if (uc != UINT32_MAX) {
                    bounds(start, uc, 64);
                    if (rt != UINT32_MAX && start < rt)
                        throw std::runtime_error("OM RTV and UAV slots overlap");
                    for (int i = 0; i < 64; ++i)
                        changes["om.uav." + std::to_string(i)] = 0;
                    auto values = arr("uavs", uc);
                    for (size_t i = 0; i < values.size(); ++i)
                        changes["om.uav." + std::to_string(start + i)] = values[i];
                    if (rt == UINT32_MAX)
                        for (uint64_t i = std::min<uint64_t>(start, 8); i < 8; ++i)
                            changes["rtv." + std::to_string(i)] = 0;
                } else if (rt != UINT32_MAX)
                    for (uint64_t i = 0; i < rt; ++i)
                        changes["om.uav." + std::to_string(i)] = 0;
                output(changes, eid);
            } else if (finishCommandListVersion(c.at("type")))
                acceptFinishCommandList(frame_, readFinishCommandList(c.at("type"), frame_.payload(eid)));
            else {
                static const std::set<uint16_t> getters{0x3528, 0x3537, 0x353d, 0x30ea,
                                                        0x31ea, 0x3353, 0x3419, 0x3531};
                static const std::set<std::string> inert{"MapCapturedWrites",
                                                         "Map",
                                                         "Unmap",
                                                         "CopyResource",
                                                         "CopySubresourceRegion",
                                                         "CopyStructureCount",
                                                         "UpdateSubresource",
                                                         "ResolveSubresource",
                                                         "GenerateMips",
                                                         "ClearDepthStencilView",
                                                         "ClearRenderTargetView",
                                                         "ClearUnorderedAccessViewUint",
                                                         "ClearUnorderedAccessViewFloat",
                                                         "Begin",
                                                         "End",
                                                         "GetData",
                                                         "Flush"};
                if (!getters.contains(c.at("type")) && !inert.contains(n))
                    throw std::runtime_error("Context command state semantics have not been recovered");
            }
        } catch (const std::exception &error) {
            invalidate(eid, error.what(), keys_);
            notes.push_back({{"event", eid}, {"reason", error.what()}});
        }
    }
    Json fields() const {
        auto rows = Json::array();
        for (const auto &key : keys_) {
            const auto &v = values_.at(key);
            rows.push_back(
                {{"field", key},
                 {"value", v},
                 {"known", !v.is_null()},
                 {"source", sources_.at(key)},
                 {"resource_id", isStateResourceField(key) && !v.is_null() && v != 0 ? v : Json(nullptr)}});
        }
        return rows;
    }
};
std::optional<Id> commandContext(const Frame &frame, const Json &command) {
    uint16_t type = command.at("type");
    if (type >= 0x3278 && type <= 0x327e)
        return {};
    Json id = command.value("object", Json(nullptr));
    if (command.value("draw", false)) {
        id = nullptr;
        for (const auto &field : command.at("fields"))
            if (field.at("name") == "context") {
                id = field.at("value");
                break;
            }
    }
    if (id.is_null())
        return {};
    try {
        auto value = id.get<Id>();
        requireImmediateContext(frame, value);
        return value;
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace
bool isStateResourceField(const std::string &key) {
    static const std::set<std::string> names{"ib",          "input_layout", "rasterizer", "blend",
                                             "depth_state", "dsv",          "predicate"};
    return names.contains(key) || key.ends_with(".shader") || key.find(".cb.") != key.npos ||
           key.find(".srv.") != key.npos || key.find(".samplers.") != key.npos ||
           key.find(".uav.") != key.npos || key.starts_with("rtv.") || key.starts_with("vb.") ||
           key.starts_with("so.targets.");
}
const std::vector<std::string> &commandStateFieldNames() {
    static const auto names = [] {
        std::vector<std::string> result;
        for (const auto &[key, value] : defaults())
            result.push_back(key);
        return result;
    }();
    return names;
}
std::optional<Id> stateCommandContext(const Frame &frame, const Json &command) {
    return commandContext(frame, command);
}
Json inspectCommandState(const Frame &frame, Id event, bool after, const Json *commands) {
    Json decoded;
    if (!commands) {
        decoded = inspectCommands(frame);
        commands = &decoded;
    }
    const Json *selected = nullptr;
    for (const auto &c : *commands)
        if (c.at("id") == event) {
            selected = &c;
            break;
        }
    if (!selected)
        throw std::runtime_error("Event is not a captured API command");
    auto context = commandContext(frame, *selected);
    Json result{{"event", event},
                {"api", selected->at("name")},
                {"context", context ? Json(*context) : Json(nullptr)},
                {"value_time", after ? "after_command" : "before_command"},
                {"source", "original_capture"},
                {"experiment_applied", false},
                {"fields", Json::array()},
                {"notes", Json::array()}};
    if (!context) {
        result["unavailable"] = "The selected record is not an identified immediate-context command";
        return result;
    }
    ContextState state(frame);
    for (const auto &command : *commands) {
        if (command.at("id").get<Id>() > event)
            break;
        if (commandContext(frame, command) != context)
            continue;
        if (command.value("draw", false))
            state.anchor(command);
        else if (command.at("id") != event || after)
            state.apply(command);
    }
    result["fields"] = state.fields();
    size_t known = 0;
    for (const auto &f : result["fields"])
        if (f["known"] == true)
            ++known;
    result["known_fields"] = known;
    result["unknown_fields"] = result["fields"].size() - known;
    result["notes"] = state.notes;
    result["limits"] = {
        "Captured state, not experiment-modified replay state",
        "Dispatch snapshot graphics shader zeros are omitted; preceding observations are retained",
        "Read-only depth/stencil and ambiguous 3D overlap are conservative unknowns",
        "SO offsets are setter arguments, not the live stream-output write cursor",
        "Hidden UAV counters require GPU resource snapshots"};
    return result;
}
Json auditCommandState(const Frame &frame) {
    std::map<Id, std::unique_ptr<ContextState>> states;
    std::vector<Id> order;
    size_t checked = 0;
    auto differences = Json::array(), notes = Json::array();
    for (const auto &command : inspectCommands(frame)) {
        auto context = commandContext(frame, command);
        if (!context)
            continue;
        if (!states.contains(*context)) {
            states[*context] = std::make_unique<ContextState>(frame);
            order.push_back(*context);
        }
        auto &state = *states.at(*context);
        if (command.value("draw", false)) {
            auto [count, changes] = state.anchor(command);
            checked += count;
            for (auto change : changes) {
                change["event"] = command["id"];
                differences.push_back(change);
            }
        } else
            state.apply(command);
    }
    for (auto id : order)
        for (const auto &note : states.at(id)->notes)
            notes.push_back(note);
    return {{"checked", checked}, {"differences", differences}, {"notes", notes}};
}
} // namespace flora
