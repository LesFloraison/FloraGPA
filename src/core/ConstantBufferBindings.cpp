#include "ConstantBufferBindings.h"
#include "Contexts.h"
#include <algorithm>
namespace flora {
std::optional<unsigned> constantBufferStage(uint16_t type) {
    if (type >= 0x249 && type <= 0x254)
        return (type - 0x249) % 6;
    auto it = std::find(constantBufferShimTypes.begin(), constantBufferShimTypes.end(), type);
    if (it == constantBufferShimTypes.end())
        return {};
    return unsigned(it - constantBufferShimTypes.begin());
}
ConstantBufferBinding readConstantBufferSetter(uint16_t type, Bytes bytes) {
    if (!constantBufferStage(type))
        throw std::runtime_error("Not a constant-buffer setter");
    Reader r(bytes);
    r.skip(8);
    ConstantBufferBinding b;
    b.type = type;
    b.context = r.read<Id>();
    b.start = r.read<uint32_t>();
    auto count = r.read<uint32_t>();
    if (b.start >= 14 || count > 14 - b.start)
        throw std::runtime_error("CB range exceeds 14 slots");
    if (!r.flag() && count)
        throw std::runtime_error("Nonempty CB binding requires an explicit array");
    for (unsigned i = 0; i < count; ++i)
        b.buffers.push_back(r.read<Id>());
    if (type >= 0x24f && type <= 0x254) {
        auto array = [&]() -> std::optional<std::vector<uint32_t>> {
            if (!r.flag())
                return {};
            std::vector<uint32_t> values;
            for (unsigned i = 0; i < count; ++i)
                values.push_back(r.read<uint32_t>());
            return values;
        };
        b.first = array();
        b.counts = array();
    }
    r.end();
    return b;
}
void validateConstantBufferBinding(const Frame &frame, const ConstantBufferBinding &b) {
    if (!constantBufferStage(b.type))
        throw std::runtime_error("Not a constant-buffer setter");
    requireImmediateContext(frame, b.context);
    if (b.start >= 14 || b.buffers.size() > 14 - b.start)
        throw std::runtime_error("CB range exceeds 14 slots");
    if (bool(b.first) != bool(b.counts))
        throw std::runtime_error("CB1 first/count arrays must both be present or absent");
    if (b.first && (b.type < 0x24f || b.type > 0x254 || b.first->size() != b.buffers.size() ||
                    b.counts->size() != b.buffers.size()))
        throw std::runtime_error("CB1 window array length or setter type mismatch");
    for (unsigned i = 0; i < b.buffers.size(); ++i) {
        if (auto id = b.buffers[i]) {
            const auto &entry = frame.entry(id);
            if (entry.category != 5 || entry.type != 0x83)
                throw std::runtime_error("CB binding requires a buffer resource");
            auto desc = frame.resource(id).desc;
            if (desc.at(2) != 4 || !desc.at(0) || desc[0] % 16)
                throw std::runtime_error("Invalid constant-buffer descriptor");
        }
        if (b.first && ((*b.first)[i] % 16 || (*b.counts)[i] % 16 || (*b.counts)[i] > 4096))
            throw std::runtime_error("CB1 windows require multiples of 16 constants and count <= 4096");
    }
}
ConstantBufferRange constantBufferRow(const ConstantBufferBinding &b, size_t row) {
    return {b.buffers.at(row), b.first ? b.first->at(row) : 0, b.counts ? b.counts->at(row) : 0,
            bool(b.first)};
}
bool displacedConstantBuffers(const ConstantBufferBinding &old, const ConstantBufferBinding &next) {
    return !old.buffers.empty() &&
           (old.start < next.start || old.start + old.buffers.size() > next.start + next.buffers.size());
}
const ConstantBufferObservations &ConstantBufferHistory::advance(Id event) {
    while (next_ != frame_.entries().end() && next_->first < event) {
        const auto &e = (next_++)->second;
        if (e.category != 7)
            continue;
        if (isDraw(e.type)) {
            auto draw = frame_.event(e.id);
            requireImmediateContext(frame_, draw.context);
            auto state = frame_.state(draw.state);
            for (unsigned stage = 0; stage < 6; ++stage)
                for (unsigned slot = 0; slot < 14; ++slot)
                    if (known_[stage][slot] && known_[stage][slot]->buffer != state.stages[stage].cb[slot])
                        known_[stage][slot].reset();
        } else if (e.type == 0x242) {
            Reader r(frame_.payload(e.id));
            r.skip(8);
            requireImmediateContext(frame_, r.read<Id>());
            for (auto &stage : known_)
                stage.fill(ConstantBufferRange{});
        } else if (auto stage = constantBufferStage(e.type)) {
            auto b = readConstantBufferSetter(e.type, frame_.payload(e.id));
            validateConstantBufferBinding(frame_, b);
            for (unsigned i = 0; i < b.buffers.size(); ++i)
                known_[*stage][b.start + i] = constantBufferRow(b, i);
        }
    }
    return known_;
}
void ConstantBufferBindings::clear() {
    for (auto &stage : active_)
        stage.clear();
}
void ConstantBufferBindings::transition(const ConstantBufferBinding &original,
                                        const ConstantBufferBinding *edited,
                                        const ConstantBufferObservations &previous) {
    const auto stage = constantBufferStage(original.type).value();
    auto next = active_[stage];
    for (unsigned slot = original.start; slot < original.start + original.buffers.size(); ++slot) {
        if (!edited)
            next.erase(slot);
        else if ((slot < edited->start || slot >= edited->start + edited->buffers.size()) &&
                 !next.contains(slot)) {
            if (!previous[stage][slot])
                throw std::runtime_error(
                    "Changed CB range requires an earlier buffer and window observation for slot " +
                    std::to_string(slot));
            next[slot] = *previous[stage][slot];
        }
    }
    if (edited)
        for (unsigned i = 0; i < edited->buffers.size(); ++i)
            next[edited->start + i] = constantBufferRow(*edited, i);
    active_[stage] = std::move(next);
}
void ConstantBufferBindings::apply(State &state) const {
    for (unsigned stage = 0; stage < 6; ++stage)
        for (const auto &[slot, range] : active_[stage]) {
            state.stages[stage].cb[slot] = range.buffer;
            state.stages[stage].cbRanges[slot] = {
                range.window ? std::optional<uint32_t>(range.first) : std::nullopt,
                range.window ? std::optional<uint32_t>(range.count) : std::nullopt};
        }
}
State effectiveConstantBufferBindings(const Frame &frame, Id event, State state,
                                      const std::map<Id, ConstantBufferBinding> &edits) {
    if (edits.empty())
        return state;
    ConstantBufferBindings tracker;
    ConstantBufferHistory history(frame);
    for (const auto &[id, e] : frame.entries()) {
        if (id >= event)
            break;
        if (e.category != 7)
            continue;
        if (e.type == 0x242)
            tracker.clear();
        else if (auto stage = constantBufferStage(e.type)) {
            auto it = edits.find(id);
            if (it == edits.end() && !tracker.active(*stage))
                continue;
            auto original = readConstantBufferSetter(e.type, frame.payload(id));
            auto edit = it == edits.end() ? nullptr : &it->second;
            validateConstantBufferBinding(frame, edit ? *edit : original);
            tracker.transition(original, edit,
                               edit && displacedConstantBuffers(original, *edit)
                                   ? history.advance(id)
                                   : ConstantBufferObservations{});
        }
    }
    tracker.apply(state);
    return state;
}
} // namespace flora
