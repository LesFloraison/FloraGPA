#include "SamplerBindings.h"
#include "Contexts.h"
#include <algorithm>
namespace flora {
std::optional<unsigned> samplerSetterStage(uint16_t type) {
    auto it = std::find(samplerSetterTypes.begin(), samplerSetterTypes.end(), type);
    if (it == samplerSetterTypes.end())
        return {};
    return unsigned(it - samplerSetterTypes.begin());
}
SamplerCommand readSamplerCommand(Bytes bytes) {
    Reader r(bytes);
    r.skip(8);
    SamplerCommand result;
    result.context = r.read<Id>();
    result.binding.start = r.read<uint32_t>();
    auto count = r.read<uint32_t>();
    if (result.binding.start >= 16 || count > 16 - result.binding.start)
        throw std::runtime_error("Sampler range exceeds 16 slots");
    const bool present = r.flag();
    if (count && !present)
        throw std::runtime_error("Nonempty sampler command requires an explicit array");
    for (uint32_t i = 0; i < count; ++i)
        result.binding.resources.push_back(r.read<Id>());
    r.end();
    return result;
}
void validateSamplerResource(const Frame &frame, Id resource) {
    if (!resource)
        return;
    const auto &entry = frame.entry(resource);
    if (entry.category != 5 || entry.type != 0x88 || frame.payload(resource).size() != 68)
        throw std::runtime_error("Sampler binding requires a 68-byte sampler-state resource");
}
void SamplerBindings::clear() {
    for (auto &stage : known_)
        stage.fill(Id(0));
    for (auto &stage : active_)
        stage.clear();
}
void SamplerBindings::observe(const State &state) {
    for (unsigned stage = 0; stage < 6; ++stage)
        for (unsigned slot = 0; slot < 16; ++slot)
            known_[stage][slot] = state.stages[stage].samplers[slot];
}
void SamplerBindings::apply(State &state) const {
    for (unsigned stage = 0; stage < 6; ++stage)
        for (const auto &[slot, id] : active_[stage])
            state.stages[stage].samplers[slot] = id;
}
void SamplerBindings::transition(unsigned stage, const SamplerBinding &original,
                                 const SamplerBinding *edited) {
    if (stage >= 6 || original.start >= 16 || original.resources.size() > 16 - original.start ||
        (edited && (edited->start >= 16 || edited->resources.size() > 16 - edited->start)))
        throw std::runtime_error("Sampler range exceeds 16 slots");
    auto next = active_[stage];
    for (unsigned slot = original.start; slot < original.start + original.resources.size(); ++slot) {
        if (!edited)
            next.erase(slot);
        else if (slot < edited->start || slot >= edited->start + edited->resources.size()) {
            if (!next.contains(slot)) {
                if (!known_[stage][slot])
                    throw std::runtime_error(
                        "Changed sampler range requires an earlier observation for original slot " +
                        std::to_string(slot));
                next[slot] = *known_[stage][slot];
            }
        }
    }
    if (edited)
        for (unsigned i = 0; i < edited->resources.size(); ++i)
            next[edited->start + i] = edited->resources[i];
    active_[stage] = std::move(next);
}
void SamplerBindings::observeCommand(unsigned stage, const SamplerBinding &original) {
    for (unsigned i = 0; i < original.resources.size(); ++i)
        known_[stage][original.start + i] = original.resources[i];
}
void SamplerBindings::observePrefix(const Frame &frame, Id event) {
    SamplerBindings prefix;
    for (const auto &[id, entry] : frame.entries()) {
        if (id >= event)
            break;
        if (entry.category != 7)
            continue;
        if (isDraw(entry.type)) {
            const auto draw = frame.event(id);
            requireImmediateContext(frame, draw.context);
            prefix.observe(frame.state(draw.state));
        } else if (entry.type == 0x242) {
            Reader r(frame.payload(id));
            r.skip(8);
            requireImmediateContext(frame, r.read<Id>());
            r.end();
            prefix.clear();
        } else if (auto stage = samplerSetterStage(entry.type)) {
            const auto command = readSamplerCommand(frame.payload(id));
            requireImmediateContext(frame, command.context);
            prefix.observeCommand(*stage, command.binding);
        }
    }
    known_ = prefix.known_;
}
State effectiveSamplerBindings(const Frame &frame, Id event, State state,
                               const std::map<Id, SamplerBinding> &edits) {
    if (edits.empty())
        return state;
    SamplerBindings tracker;
    for (const auto &[id, entry] : frame.entries()) {
        if (id >= event)
            break;
        if (entry.category != 7)
            continue;
        if (entry.type == 0x242)
            tracker.clear();
        else if (auto stage = samplerSetterStage(entry.type)) {
            auto edit = edits.find(id);
            if (edit == edits.end() && !tracker.active(*stage))
                continue;
            const auto command = readSamplerCommand(frame.payload(id));
            requireImmediateContext(frame, command.context);
            if (edit == edits.end()) {
                for (auto resource : command.binding.resources)
                    validateSamplerResource(frame, resource);
            } else {
                const auto &old = command.binding;
                const auto &next = edit->second;
                if (!old.resources.empty() &&
                    (old.start < next.start ||
                     old.start + old.resources.size() > next.start + next.resources.size()))
                    tracker.observePrefix(frame, id);
            }
            tracker.transition(*stage, command.binding, edit == edits.end() ? nullptr : &edit->second);
        }
    }
    tracker.apply(state);
    return state;
}
} // namespace flora
