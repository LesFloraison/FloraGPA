#include "SrvBindings.h"
#include "Contexts.h"
#include "OutputBindings.h"
#include "StreamOutput.h"
#include <algorithm>
namespace flora {
std::optional<unsigned> srvSetterStage(uint16_t type) {
    auto it = std::find(srvSetterTypes.begin(), srvSetterTypes.end(), type);
    if (it == srvSetterTypes.end())
        return {};
    return unsigned(it - srvSetterTypes.begin());
}
SrvCommand readSrvCommand(Bytes bytes) {
    Reader r(bytes);
    r.skip(8);
    SrvCommand result;
    result.context = r.read<Id>();
    result.binding.start = r.read<uint32_t>();
    auto count = r.read<uint32_t>();
    if (result.binding.start >= 128 || count > 128 - result.binding.start)
        throw std::runtime_error("SRV range exceeds 128 slots");
    if (!r.flag() && count)
        throw std::runtime_error("Nonempty SRV command requires an explicit array");
    for (uint32_t i = 0; i < count; ++i)
        result.binding.views.push_back(r.read<Id>());
    r.end();
    return result;
}
void validateSrvResource(const Frame &frame, Id view) {
    if (!view)
        return;
    Reader r(frame.payload(view, 5, 0x8c));
    r.skip(16);
    auto owner = frame.resource(r.read<Id>());
    r.skip(24);
    r.end();
    if (owner.type < 0x83 || owner.type > 0x87)
        throw std::runtime_error("SRV owner is not a supported buffer or texture");
}
bool isSrvOutputCommand(uint16_t type) { return isOutputCommand(type) || isStreamOutputTargets(type); }
SrvOutputs srvOutputs(const State &s) {
    SrvOutputs out{};
    for (unsigned i = 0; i < 8; ++i)
        out[i] = i < std::min(s.rtCount, s.omStart) ? s.rtv[i] : 0;
    for (unsigned i = 0; i < 64; ++i) {
        out[8 + i] = i >= s.omStart && i < s.rtCount ? (i < 8 ? s.rtv[i] : s.omExtended[i - 8]) : 0;
        out[72 + i] = i < 8 ? s.csUav[i] : s.csExtended[i - 8];
    }
    for (unsigned i = 0; i < 4; ++i)
        out[136 + i] = i < s.soCount ? s.so[i] : 0;
    out[140] = s.dsv;
    return out;
}
std::optional<SrvHazards::Span> SrvHazards::span(Id id) {
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

std::optional<bool> SrvHazards::overlap(std::optional<Id> a, std::optional<Id> b, bool shaderResource,
                                        bool wholeSubresource) {
    if (a == Id(0) || b == Id(0))
        return false;
    if (!a || !b)
        return {};
    try {
        auto left = span(*a), right = span(*b);
        if (!left || !right)
            return {};
        if (left->resource != right->resource)
            return false;
        if (left->subresources && right->subresources) {
            bool hit = false;
            for (auto sub : *left->subresources)
                if (right->subresources->contains(sub)) {
                    hit = true;
                    break;
                }
            if (!hit)
                return false;
        }
        if (shaderResource) {
            if (frame_.entry(*b).type == 0x8e) {
                Reader srv(frame_.payload(*a));
                srv.skip(24);
                auto format = srv.read<uint32_t>();
                Reader dsv(frame_.payload(*b));
                dsv.skip(24);
                auto depth = dsv.read<uint32_t>();
                dsv.skip(4);
                auto flags = dsv.read<uint32_t>();
                unsigned plane = 0;
                if ((depth == 55 && format == 56) || (depth == 40 && format == 41) ||
                    (depth == 45 && format == 46) || (depth == 20 && format == 21))
                    plane = 1;
                if ((depth == 45 && format == 47) || (depth == 20 && format == 22))
                    plane = 2;
                if (!plane)
                    return {};
                return !(flags & plane);
            }
            // W slices of a 3D mip do not partition shader-resource access.
            return true;
        }
        if (!wholeSubresource && (left->ambiguous || right->ambiguous))
            return {};
        return true;
    } catch (const std::exception &) {
        return {};
    }
}
std::optional<Id> SrvHazards::effective(Id view, const SrvOutputs &outputs, bool unknown) {
    if (!view)
        return Id(0);
    if (!frame_.entries().contains(view)) {
        if (unknown)
            return {};
        throw std::runtime_error("Captured SRV record is absent: " + std::to_string(view));
    }
    Reader r(frame_.payload(view));
    r.skip(16);
    const auto resource = frame_.resource(r.read<Id>());
    unsigned index = resource.type == 0x83 ? 2 : resource.type == 0x84 ? 5 : resource.type == 0x86 ? 6 : 8;
    const auto flags = resource.desc.at(index);
    bool uncertain = false;
    for (unsigned i = 0; i < outputs.size(); ++i) {
        unsigned flag = i < 8 ? 32 : i < 136 ? 128 : i < 140 ? 16 : 64;
        if (!(flags & flag) || outputs[i] == Id(0))
            continue;
        auto result = overlap(view, outputs[i], true);
        if (result == true)
            return Id(0);
        uncertain |= !result.has_value();
    }
    if (uncertain) {
        if (unknown)
            return {};
        throw std::runtime_error(
            "SRV input/output conflict cannot be determined from preceding output observations");
    }
    return view;
}
namespace {
std::map<unsigned, Id> outputChanges(const Frame &frame, const Entry &entry) {
    std::map<unsigned, Id> out;
    if (isStreamOutputTargets(entry.type)) {
        const auto c = readStreamOutputTargets(frame.payload(entry.id));
        if (c.count > 4)
            throw std::runtime_error("SO count exceeds four");
        for (unsigned i = 0; i < 4; ++i)
            out[136 + i] = c.buffers && i < c.buffers->size() ? c.buffers->at(i) : 0;
        return out;
    }
    return outputChanges(entry.type, readOutputCommand(entry.type, frame.payload(entry.id)));
}
} // namespace
SrvHistory::SrvHistory(const Frame &frame, Id context)
    : frame_(frame), context_(context), next_(frame.entries().begin()), hazards_(frame) {
    requireImmediateContext(frame, context);
}
void SrvHistory::apply(const Entry &entry) {
    if (isDraw(entry.type)) {
        auto s = frame_.state(frame_.event(entry.id).state);
        state_.outputs = srvOutputs(s);
        for (unsigned st = 0; st < 6; ++st)
            for (unsigned slot = 0; slot < 128; ++slot)
                state_.srvs[st][slot] = s.stages[st].srv[slot];
    } else if (entry.type == 0x242) {
        try {
            Reader r(frame_.payload(entry.id));
            r.skip(16);
            r.end();
            state_.outputs.fill(Id(0));
            for (auto &bindings : state_.srvs)
                bindings.fill(Id(0));
        } catch (const std::exception &) {
            state_ = SrvObservation{};
        }
    } else if (auto stage = srvSetterStage(entry.type)) {
        const auto c = readSrvCommand(frame_.payload(entry.id));
        try {
            for (unsigned i = 0; i < c.binding.views.size(); ++i)
                state_.srvs[*stage][c.binding.start + i] =
                    hazards_.effective(c.binding.views[i], state_.outputs, true);
        } catch (const std::exception &) {
            state_ = SrvObservation{};
        }
    } else {
        try {
            auto changes = outputChanges(frame_, entry);
            std::vector<Id> ids;
            for (const auto &[slot, id] : changes)
                if (id)
                    ids.push_back(id);
            for (size_t i = 0; i < ids.size(); ++i)
                for (size_t j = i + 1; j < ids.size(); ++j)
                    if (hazards_.overlap(ids[i], ids[j]) != false)
                        throw std::runtime_error("Overlapping or unresolved output views");
            for (unsigned slot = 0; slot < state_.outputs.size(); ++slot) {
                if (changes.contains(slot))
                    continue;
                bool conflict = false, unknown = false;
                for (auto id : ids) {
                    auto r = hazards_.overlap(state_.outputs[slot], id);
                    conflict |= r == true;
                    unknown |= !r.has_value();
                }
                if (conflict)
                    state_.outputs[slot] = 0;
                else if (unknown)
                    state_.outputs[slot].reset();
            }
            for (const auto &[slot, id] : changes)
                state_.outputs[slot] = id;
            for (auto &bindings : state_.srvs)
                for (auto &id : bindings)
                    if (id)
                        id = hazards_.effective(*id, state_.outputs, true);
        } catch (const std::exception &) {
            state_ = SrvObservation{};
        }
    }
}
const SrvObservation &SrvHistory::advance(Id event, bool after) {
    while (next_ != frame_.entries().end()) {
        const auto &entry = next_->second;
        if (entry.id > event || (entry.id == event && !after))
            break;
        ++next_;
        if (entry.category != 7 || (!isDraw(entry.type) && entry.type != 0x242 &&
                                    !srvSetterStage(entry.type) && !isSrvOutputCommand(entry.type)))
            continue;
        Reader r(frame_.payload(entry.id));
        r.skip(isDraw(entry.type) ? 16 : 8);
        if (r.read<Id>() == context_)
            apply(entry);
    }
    return state_;
}
void SrvBindings::clear() {
    for (auto &stage : active_)
        stage.clear();
}
bool SrvBindings::active() const {
    return std::any_of(active_.begin(), active_.end(), [](const auto &s) { return !s.empty(); });
}
void SrvBindings::transition(unsigned stage, const SrvBinding &original, const SrvBinding *edited,
                             const SrvObservation &previous) {
    if (stage >= 6 || original.start >= 128 || original.views.size() > 128 - original.start ||
        (edited && (edited->start >= 128 || edited->views.size() > 128 - edited->start)))
        throw std::runtime_error("SRV range exceeds 128 slots");
    auto next = active_[stage];
    for (unsigned i = original.start; i < original.start + original.views.size(); ++i) {
        if (!edited)
            next.erase(i);
        else if ((i < edited->start || i >= edited->start + edited->views.size()) && !next.contains(i)) {
            if (!previous.srvs[stage][i])
                throw std::runtime_error("Moved SRV range requires an earlier resolved slot observation");
            next[i] = *previous.srvs[stage][i];
        }
    }
    if (edited)
        for (unsigned i = 0; i < edited->views.size(); ++i)
            next[edited->start + i] = edited->views[i];
    active_[stage] = std::move(next);
}
void SrvBindings::hazards(SrvHazards &checker, const SrvOutputs &outputs) {
    auto next = active_;
    for (auto &stage : next)
        for (auto &[slot, id] : stage)
            id = *checker.effective(id, outputs);
    active_ = std::move(next);
}
void SrvBindings::apply(State &state) const {
    for (unsigned st = 0; st < 6; ++st)
        for (const auto &[slot, id] : active_[st])
            state.stages[st].srv[slot] = id;
}
void SrvBindings::unbind(unsigned stage, unsigned slot) {
    if (active_.at(stage).contains(slot))
        active_[stage][slot] = 0;
}
State effectiveSrvBindings(const Frame &frame, Id event, State state, const std::map<Id, SrvBinding> &edits) {
    if (edits.empty())
        return state;
    requireImmediateContext(frame, frame.event(event).context);
    std::map<Id, std::unique_ptr<SrvHistory>> histories;
    auto history = [&](Id context, Id id, bool after = false) -> const SrvObservation & {
        auto &value = histories[context];
        if (!value)
            value = std::make_unique<SrvHistory>(frame, context);
        return value->advance(id, after);
    };
    SrvHazards checker(frame);
    SrvBindings tracker;
    for (const auto &[id, entry] : frame.entries()) {
        if (id >= event)
            break;
        if (entry.category != 7)
            continue;
        // Replay maps recovered immediate-context records onto its native context.
        // Retain the same command order here; only captured prefix observations are per context.
        if (entry.type == 0x242)
            tracker.clear();
        else if (auto stage = srvSetterStage(entry.type)) {
            auto edit = edits.find(id);
            if (edit == edits.end() && !tracker.active(*stage))
                continue;
            const auto command = readSrvCommand(frame.payload(id));
            requireImmediateContext(frame, command.context);
            if (edit == edits.end())
                for (auto view : command.binding.views)
                    if (!view || frame.entries().contains(view))
                        validateSrvResource(frame, view);
            const auto &previous = history(command.context, id);
            tracker.transition(*stage, command.binding, edit == edits.end() ? nullptr : &edit->second,
                               previous);
            tracker.hazards(checker, previous.outputs);
        } else if (tracker.active() && isDraw(entry.type)) {
            tracker.hazards(checker, srvOutputs(frame.state(frame.event(id).state)));
        } else if (tracker.active() && isSrvOutputCommand(entry.type)) {
            Reader owner(frame.payload(id));
            owner.skip(8);
            tracker.hazards(checker, history(owner.read<Id>(), id, true).outputs);
        }
    }
    tracker.hazards(checker, srvOutputs(state));
    tracker.apply(state);
    return state;
}
} // namespace flora
