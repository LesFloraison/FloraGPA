#include "OutputBindings.h"
#include <algorithm>
namespace flora {
namespace {
template <class T>
std::optional<std::vector<T>> array(Reader &r, uint32_t count, uint32_t limit, bool required = false) {
    if (!r.flag()) {
        if (required && count)
            throw std::runtime_error("Nonempty binding requires an explicit array");
        return {};
    }
    if (count > limit)
        throw std::runtime_error("Binding array exceeds slot limit or KEEP requires an absent array");
    std::vector<T> out;
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(r.read<T>());
    return out;
}
const std::array<std::string, 141> &outputKeys() {
    static const auto keys = [] {
        std::array<std::string, 141> out;
        for (unsigned i = 0; i < 8; ++i)
            out[i] = "rtv." + std::to_string(i);
        for (unsigned i = 0; i < 64; ++i) {
            out[8 + i] = "om.uav." + std::to_string(i);
            out[72 + i] = "cs.uav." + std::to_string(i);
        }
        for (unsigned i = 0; i < 4; ++i)
            out[136 + i] = "so.targets." + std::to_string(i);
        out[140] = "dsv";
        return out;
    }();
    return keys;
}
const std::array<std::string, 801> &inputKeys() {
    static const auto keys = [] {
        std::array<std::string, 801> out;
        out[0] = "ib";
        for (unsigned i = 0; i < 32; ++i)
            out[1 + i] = "vb." + std::to_string(i);
        constexpr const char *stages[]{"vs", "hs", "ds", "gs", "ps", "cs"};
        for (unsigned stage = 0; stage < 6; ++stage)
            for (unsigned i = 0; i < 128; ++i)
                out[33 + stage * 128 + i] = std::string(stages[stage]) + ".srv." + std::to_string(i);
        return out;
    }();
    return keys;
}
bool inputKey(const std::string &key) {
    return key == "ib" || key.starts_with("vb.") || key.find(".srv.") != std::string::npos;
}
bool outputKey(const std::string &key) {
    const auto &keys = outputKeys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}
SrvOutputs outputs(const BindingValues &values) {
    SrvOutputs out;
    for (unsigned i = 0; i < out.size(); ++i)
        out[i] = values.at(outputKeys()[i]);
    return out;
}
BindingValues namedChanges(const std::map<unsigned, Id> &changes) {
    BindingValues out;
    for (auto [i, value] : changes)
        out[outputKeys().at(i)] = value;
    return out;
}
void clearIaMetadata(BindingValues &values, const std::string &key) {
    if (key == "ib")
        values["ib_format"] = values["ib_offset"] = 0;
    else if (key.starts_with("vb.")) {
        auto suffix = key.substr(3);
        values["strides." + suffix] = values["offsets." + suffix] = 0;
    }
}
std::array<int64_t, 6> targetSignature(const Frame &frame, Id id) {
    Reader r(frame.payload(id));
    r.skip(16);
    auto resource = frame.resource(r.read<Id>());
    std::vector<uint32_t> fields;
    while (r.remaining())
        fields.push_back(r.read<uint32_t>());
    if (resource.type == 0x83)
        return {1, fields.at(3), 1, 1, 1, 0};
    auto info = textureInfo(resource);
    auto dim = fields.at(1);
    uint32_t mip = 0;
    int64_t layers = 1;
    if (frame.entry(id).type == 0x8e) {
        if (dim >= 1 && dim <= 4)
            mip = fields.at(3);
        if (dim == 2 || dim == 4)
            layers = fields.at(5);
        else if (dim == 6)
            layers = fields.at(4);
    } else {
        if (dim == 2 || dim == 3 || dim == 4 || dim == 5 || dim == 8)
            mip = fields.at(2);
        if (dim == 3 || dim == 5 || dim == 8)
            layers = fields.at(4);
        else if (dim == 7)
            layers = fields.at(3);
        if (dim == 8 && layers == keepOutput)
            layers = int64_t(mip >= 32 ? 1 : std::max(1u, info.depth >> mip)) - fields.at(3);
    }
    return {info.dimension,
            mip >= 32 ? 1 : std::max(1u, info.width >> mip),
            mip >= 32 ? 1 : std::max(1u, info.height >> mip),
            layers,
            info.samples,
            info.dimension == 3 ? resource.desc.at(6) : 0};
}
} // namespace
bool isOutputCommand(uint16_t type) {
    return type == 0x34ff || type == 0x3500 || type == 0x3522 || type == 0x25e;
}
OutputCommand readOutputCommand(uint16_t type, Bytes bytes) {
    if (!isOutputCommand(type))
        throw std::runtime_error("Unsupported output command");
    Reader r(bytes);
    r.skip(8);
    OutputCommand out;
    out.context = r.read<Id>();
    if (type == 0x34ff || type == 0x3500) {
        out.rtvCount = r.read<uint32_t>();
        if (out.rtvCount > 8 && !(type == 0x3500 && out.rtvCount == keepOutput))
            throw std::runtime_error("RTV count exceeds eight");
        out.rtvs = array<Id>(r, out.rtvCount, 8, out.rtvCount != keepOutput);
        out.dsv = r.read<Id>();
    }
    if (type != 0x34ff) {
        out.start = r.read<uint32_t>();
        out.uavCount = r.read<uint32_t>();
        if (out.uavCount > 64 && !(type == 0x3500 && out.uavCount == keepOutput))
            throw std::runtime_error("UAV count exceeds 64");
        out.uavs = array<Id>(r, out.uavCount, 64, out.uavCount != keepOutput);
        out.initialCounts = array<uint32_t>(r, out.uavCount, 64);
    }
    r.end();
    return out;
}
void validateOutputRange(uint16_t type, const OutputCommand &c, uint32_t limit) {
    if (!isOutputCommand(type) || limit > 64)
        throw std::runtime_error("Invalid output command or slot limit");
    auto checkArray = [](const auto &values, uint32_t count, bool required) {
        if (count == keepOutput) {
            if (values)
                throw std::runtime_error("KEEP output requires absent arrays");
        } else if ((values && values->size() != count) || (required && count && !values))
            throw std::runtime_error("Output array does not match count");
    };
    if (type == 0x34ff || type == 0x3500) {
        if (c.rtvCount > 8 && !(type == 0x3500 && c.rtvCount == keepOutput))
            throw std::runtime_error("RTV count exceeds eight");
        checkArray(c.rtvs, c.rtvCount, true);
    }
    if (type != 0x34ff) {
        if (c.uavCount > 64 && !(type == 0x3500 && c.uavCount == keepOutput))
            throw std::runtime_error("UAV count exceeds 64");
        checkArray(c.uavs, c.uavCount, true);
        checkArray(c.initialCounts, c.uavCount, false);
    }
    if (type != 0x34ff && c.uavCount != keepOutput) {
        if (c.start > limit || c.uavCount > limit - c.start)
            throw std::runtime_error("UAV binding exceeds slot limit");
        if (type == 0x3500 && c.rtvCount != keepOutput && c.start < c.rtvCount)
            throw std::runtime_error("OM RTV and UAV slots overlap");
    }
}
std::map<unsigned, Id> outputChanges(uint16_t type, const OutputCommand &c) {
    validateOutputRange(type, c);
    std::map<unsigned, Id> out;
    bool om = type == 0x34ff || type == 0x3500;
    if (om && c.rtvCount != keepOutput) {
        for (unsigned i = 0; i < 8; ++i)
            out[i] = c.rtvs && i < c.rtvs->size() ? c.rtvs->at(i) : 0;
        out[140] = c.dsv;
    }
    if (type == 0x34ff) {
        for (unsigned i = 0; i < 64; ++i)
            out[8 + i] = 0;
    } else if (om) {
        if (c.uavCount != keepOutput) {
            for (unsigned i = 0; i < 64; ++i)
                out[8 + i] = 0;
            for (unsigned i = 0; i < c.uavCount; ++i)
                out[8 + c.start + i] = c.uavs->at(i);
            if (c.rtvCount == keepOutput)
                for (unsigned i = std::min(c.start, 8u); i < 8; ++i)
                    out[i] = 0;
        } else if (c.rtvCount != keepOutput)
            for (unsigned i = 0; i < c.rtvCount; ++i)
                out[8 + i] = 0;
    } else
        for (unsigned i = 0; i < c.uavCount; ++i)
            out[72 + c.start + i] = c.uavs->at(i);
    return out;
}
void validateOutputTargets(const Frame &frame, const SrvOutputs &bindings) {
    std::optional<std::array<int64_t, 6>> first;
    for (unsigned i = 0; i < bindings.size(); ++i) {
        if (i >= 8 && i != 140)
            continue;
        if (!bindings[i] || !*bindings[i])
            continue;
        auto signature = targetSignature(frame, *bindings[i]);
        if (first && *first != signature)
            throw std::runtime_error(
                "RTV/DSV effective dimensions, resource types and sample parameters must match");
        first = signature;
    }
}
BindingValues OutputBindingModel::snapshot(const State &s) {
    BindingValues out;
    auto target = srvOutputs(s);
    for (unsigned i = 0; i < target.size(); ++i)
        out[outputKeys()[i]] = target[i];
    out["ib"] = s.ib;
    out["ib_format"] = s.ibFormat;
    out["ib_offset"] = s.ibOffset;
    for (unsigned i = 0; i < 32; ++i) {
        out[inputKeys()[1 + i]] = s.vb[i];
        out["strides." + std::to_string(i)] = s.strides[i];
        out["offsets." + std::to_string(i)] = s.offsets[i];
    }
    for (unsigned st = 0; st < 6; ++st)
        for (unsigned i = 0; i < 128; ++i)
            out[inputKeys()[33 + st * 128 + i]] = s.stages[st].srv[i];
    return out;
}
OutputBindingModel::OutputBindingModel(const Frame &frame, Id context)
    : frame_(frame), context_(context), hazards_(frame), original_(snapshot(State{})) {
    for (auto &[key, value] : original_)
        value.reset();
    changed_ = original_;
}
bool OutputBindingModel::models(uint16_t type) {
    return isSrvOutputCommand(type) || srvSetterStage(type) || isDraw(type) || type == 0x242 ||
           type == 0x34ef || type == 0x34f0 || type == 0x34f1;
}
bool OutputBindingModel::possible(std::optional<Id> id, std::optional<Id> other, const std::string &role,
                                  const std::string &key) {
    if (id == Id(0))
        return false;
    if (!id)
        return true;
    try {
        auto entry = frame_.entry(*id);
        Id owner = *id;
        if (entry.type != 0x83) {
            Reader r(frame_.payload(*id));
            r.skip(16);
            owner = r.read<Id>();
        }
        auto resource = frame_.resource(owner);
        unsigned index = resource.type == 0x83   ? 2
                         : resource.type == 0x84 ? 5
                         : resource.type == 0x86 ? 6
                                                 : 8;
        unsigned flag = role == "dsv"                     ? 64
                        : role.starts_with("rtv.")        ? 32
                        : role.starts_with("so.targets.") ? 16
                                                          : 128;
        if (!(resource.desc.at(index) & flag))
            return false;
        if (!other || !frame_.entries().contains(*other))
            return true;
        return key.find(".srv.") != std::string::npos ? hazards_.overlap(id, other, true) != false
                                                      : hazards_.outputOverlap(id, other) != false;
    } catch (const std::exception &) {
        return true;
    }
}
std::optional<Id> OutputBindingModel::iaEffective(Id id, const SrvOutputs &bindings) {
    if (!id)
        return Id(0);
    auto flags = frame_.resource(id).desc.at(2);
    bool uncertain = false;
    for (unsigned i = 0; i < bindings.size(); ++i) {
        unsigned flag = i < 8 ? 32 : i < 136 ? 128 : i < 140 ? 16 : 0;
        auto value = bindings[i];
        if (!(flags & flag) || value == Id(0))
            continue;
        if (!value || !frame_.entries().contains(*value)) {
            uncertain = true;
            continue;
        }
        Id owner = *value;
        if (flag != 16) {
            Reader r(frame_.payload(*value));
            r.skip(16);
            owner = r.read<Id>();
        }
        if (owner == id)
            return Id(0);
    }
    return uncertain ? std::optional<Id>{} : std::optional<Id>{id};
}
void OutputBindingModel::inputs(BindingValues &values) {
    auto target = outputs(values);
    for (const auto &key : inputKeys()) {
        auto previous = values.at(key);
        if (!previous)
            continue;
        auto actual = key.find(".srv.") != std::string::npos ? hazards_.effective(*previous, target, true)
                                                             : iaEffective(*previous, target);
        values[key] = actual;
        if (*previous && actual == Id(0))
            clearIaMetadata(values, key);
    }
}
void OutputBindingModel::output(BindingValues &values, const BindingValues &changes) {
    std::vector<Id> incoming;
    for (auto &[key, id] : changes)
        if (id && *id)
            incoming.push_back(*id);
    for (size_t i = 0; i < incoming.size(); ++i)
        for (size_t j = i + 1; j < incoming.size(); ++j)
            if (hazards_.outputOverlap(incoming[i], incoming[j]) != false)
                throw std::runtime_error("Simultaneous output views overlap or cannot be resolved");
    for (const auto &key : outputKeys()) {
        if (changes.contains(key))
            continue;
        bool conflict = false, unknown = false;
        for (auto id : incoming) {
            auto overlap = hazards_.outputOverlap(values.at(key), id);
            conflict |= overlap == true;
            unknown |= !overlap;
        }
        if (conflict)
            values[key] = 0;
        else if (unknown)
            values[key].reset();
    }
    for (auto &[key, value] : changes)
        values[key] = value;
    validateOutputTargets(frame_, outputs(values));
    inputs(values);
}
std::set<std::string> OutputBindingModel::apply(BindingValues &values, uint16_t type, Bytes bytes) {
    BindingValues changes;
    if (isOutputCommand(type)) {
        auto c = readOutputCommand(type, bytes);
        validateOutputRange(type, c);
        std::vector<std::pair<Id, uint16_t>> used;
        if ((type == 0x34ff || type == 0x3500) && c.rtvCount != keepOutput) {
            if (c.rtvs)
                for (auto id : *c.rtvs)
                    used.emplace_back(id, uint16_t(0x8d));
            used.emplace_back(c.dsv, uint16_t(0x8e));
        }
        if (type != 0x34ff && c.uavCount != keepOutput && c.uavs)
            for (auto id : *c.uavs)
                used.emplace_back(id, uint16_t(0x8f));
        std::set<Id> missing;
        for (auto [id, expected] : used)
            if (id && !frame_.entries().contains(id))
                missing.insert(id);
        if (!missing.empty())
            throw MissingOutputResource(std::move(missing));
        for (auto [id, expected] : used)
            if (id) {
                const auto &e = frame_.entry(id);
                if (e.category != 5 || e.type != expected)
                    throw std::runtime_error("Output binding view type mismatch");
                if (!hazards_.outputOverlap(id, id))
                    throw std::runtime_error("Output view subresources cannot be resolved");
            }
        changes = namedChanges(outputChanges(type, c));
        output(values, changes);
    } else if (isStreamOutputTargets(type)) {
        auto c = readStreamOutputTargets(bytes);
        for (unsigned i = 0; i < 4; ++i)
            changes[outputKeys()[136 + i]] = c.buffers && i < c.buffers->size() ? c.buffers->at(i) : 0;
        output(values, changes);
    } else if (auto stage = srvSetterStage(type)) {
        auto c = readSrvCommand(bytes);
        for (unsigned i = 0; i < c.binding.views.size(); ++i)
            changes[inputKeys()[33 + *stage * 128 + c.binding.start + i]] =
                hazards_.effective(c.binding.views[i], outputs(values), true);
    } else if (type == 0x34f0) {
        Reader r(bytes);
        r.skip(16);
        auto start = r.read<uint32_t>(), count = r.read<uint32_t>();
        if (start > 31 || count > 32 - start)
            throw std::runtime_error("IA vertex slots exceed 32");
        auto ids = array<Id>(r, count, 32, true);
        auto strides = array<uint32_t>(r, count, 32, true), offsets = array<uint32_t>(r, count, 32, true);
        r.end();
        for (unsigned i = 0; i < count; ++i) {
            auto suffix = std::to_string(start + i);
            changes["vb." + suffix] = iaEffective(ids->at(i), outputs(values));
            changes["strides." + suffix] = strides->at(i);
            changes["offsets." + suffix] = offsets->at(i);
        }
    } else if (type == 0x34f1) {
        Reader r(bytes);
        r.skip(16);
        changes["ib"] = iaEffective(r.read<Id>(), outputs(values));
        changes["ib_format"] = r.read<uint32_t>();
        changes["ib_offset"] = r.read<uint32_t>();
        r.end();
    } else if (type == 0x34ef) {
        Reader r(bytes);
        r.skip(24);
        r.end();
    }
    std::set<std::string> writes;
    for (auto &[key, value] : changes) {
        values[key] = value;
        writes.insert(key);
    }
    return writes;
}
void OutputBindingModel::differences() {
    std::erase_if(uncertain_, [&](const auto &key) { return original_.at(key) && changed_.at(key); });
    dirty_ = protected_;
    dirty_.insert(uncertain_.begin(), uncertain_.end());
    for (auto &[key, value] : original_)
        if (value != changed_.at(key))
            dirty_.insert(key);
}
void OutputBindingModel::step(const Entry &entry, std::optional<Bytes> replacement) {
    if (!models(entry.type))
        return;
    auto bytes = frame_.payload(entry.id);
    Reader r(bytes);
    r.skip(isDraw(entry.type) ? 16 : 8);
    if (r.read<Id>() != context_)
        return;
    if (isDraw(entry.type)) {
        anchor(frame_.state(frame_.event(entry.id).state));
        return;
    }
    if (entry.type == 0x242) {
        r.end();
        original_ = snapshot(State{});
        changed_ = original_;
        protected_.clear();
        uncertain_.clear();
        dirty_.clear();
        return;
    }
    if (replacement && isSrvOutputCommand(entry.type)) {
        for (auto &[key, value] : changed_)
            if (!value)
                throw std::runtime_error("Output edit needs a resolved preceding binding context: " + key);
        for (auto &key : outputKeys())
            if (*changed_.at(key) && !hazards_.outputOverlap(changed_.at(key), changed_.at(key)))
                throw std::runtime_error("Preceding output view cannot be resolved");
    }
    auto original = original_, changed = changed_;
    auto oldWrites = apply(original, entry.type, bytes);
    auto newWrites = apply(changed, entry.type, replacement.value_or(bytes));
    if (replacement) {
        oldWrites.insert(newWrites.begin(), newWrites.end());
        for (auto &key : oldWrites)
            if (!changed.at(key))
                throw std::runtime_error("Changed binding range depends on an unknown earlier observation");
        protected_.insert(oldWrites.begin(), oldWrites.end());
    } else {
        // Only resource input keys participate in uncertain collateral effects.
        // Resolve their original operands without applying hazards a second time.
        auto operands = snapshot(State{});
        apply(operands, entry.type, bytes);
        for (auto &key : newWrites) {
            protected_.erase(key);
            if (!inputKey(key))
                continue;
            bool unknown = false;
            if (!original.at(key) || !changed.at(key))
                for (auto &role : uncertain_)
                    if (outputKey(role) && possible(operands.at(key), {}, role, key)) {
                        unknown = true;
                        break;
                    }
            if (unknown)
                uncertain_.insert(key);
            else
                uncertain_.erase(key);
        }
    }
    original_ = std::move(original);
    changed_ = std::move(changed);
    differences();
}
BindingGap OutputBindingModel::gap(const Entry &entry) {
    const auto changes =
        namedChanges(outputChanges(entry.type, readOutputCommand(entry.type, frame_.payload(entry.id))));
    BindingGap result;
    auto invalidate = [&](BindingValues values) {
        std::set<std::string> keys;
        for (auto &[key, value] : changes)
            if (!(values.at(key) == Id(0) && value == Id(0)))
                keys.insert(key);
        for (auto &[key, value] : values)
            if ((inputKey(key) || outputKey(key)) && !keys.contains(key))
                for (auto &[role, id] : changes)
                    if (id && *id && possible(value, id, role, key)) {
                        keys.insert(key);
                        break;
                    }
        auto expanded = keys;
        for (auto &key : keys) {
            if (key == "ib") {
                expanded.insert("ib_format");
                expanded.insert("ib_offset");
            } else if (key.starts_with("vb.")) {
                expanded.insert("strides." + key.substr(3));
                expanded.insert("offsets." + key.substr(3));
            }
        }
        for (auto &key : expanded)
            values[key].reset();
        result.affected.insert(expanded.begin(), expanded.end());
        return values;
    };
    auto original = invalidate(original_), changed = invalidate(changed_);
    std::set<std::string> common;
    for (auto &[key, value] : changes)
        if (!uncertain_.contains(key) && original_.at(key) && original_.at(key) == changed_.at(key))
            common.insert(key);
    for (auto &key : dirty_)
        if (!common.contains(key) && result.affected.contains(key))
            result.edited.insert(key);
    uncertain_.insert(result.edited.begin(), result.edited.end());
    for (auto &key : common)
        protected_.erase(key);
    original_ = std::move(original);
    changed_ = std::move(changed);
    differences();
    return result;
}
void OutputBindingModel::anchor(const State &state) {
    auto actual = snapshot(state), changed = changed_;
    for (auto &[key, value] : changed) {
        if (!dirty_.contains(key))
            value = actual.at(key);
        else if (!value)
            throw std::runtime_error("Edited binding cannot be reconstructed at snapshot: " + key);
    }
    BindingValues retained;
    for (auto &key : dirty_)
        if (outputKey(key))
            retained[key] = changed.at(key);
    output(changed, retained);
    original_ = std::move(actual);
    changed_ = std::move(changed);
    differences();
}
State OutputBindingModel::overlay(State s) const {
    for (auto &[key, value] : changed_)
        if (!value)
            throw std::runtime_error("Binding context is not fully resolved");
    auto get = [&](const std::string &key) { return *changed_.at(key); };
    s.ib = get("ib");
    s.ibFormat = uint32_t(get("ib_format"));
    s.ibOffset = uint32_t(get("ib_offset"));
    for (unsigned i = 0; i < 32; ++i) {
        s.vb[i] = get(inputKeys()[1 + i]);
        s.strides[i] = uint32_t(get("strides." + std::to_string(i)));
        s.offsets[i] = uint32_t(get("offsets." + std::to_string(i)));
    }
    for (unsigned st = 0; st < 6; ++st)
        for (unsigned i = 0; i < 128; ++i)
            s.stages[st].srv[i] = get(inputKeys()[33 + st * 128 + i]);
    s.rtCount = 0;
    s.omStart = 64;
    for (unsigned i = 0; i < 64; ++i) {
        Id rt = i < 8 ? get(outputKeys()[i]) : 0, uav = get(outputKeys()[8 + i]);
        if (rt && uav)
            throw std::runtime_error("RTV and UAV occupy the same bind point");
        Id merged = rt ? rt : uav;
        if (uav)
            s.omStart = std::min(s.omStart, i);
        if (merged)
            s.rtCount = i + 1;
        (i < 8 ? s.rtv[i] : s.omExtended[i - 8]) = merged;
        (i < 8 ? s.csUav[i] : s.csExtended[i - 8]) = get(outputKeys()[72 + i]);
    }
    s.dsv = get("dsv");
    s.soCount = 0;
    for (unsigned i = 0; i < 4; ++i) {
        s.so[i] = get(outputKeys()[136 + i]);
        if (s.so[i])
            s.soCount = i + 1;
    }
    return s;
}
} // namespace flora
