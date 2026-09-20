#include "IaBindings.h"
#include "Contexts.h"
namespace flora {
bool isIaSetter(uint16_t type) { return type >= 0x34ef && type <= 0x34f1; }
IaBinding readIaSetter(uint16_t type, Bytes bytes) {
    if (!isIaSetter(type))
        throw std::runtime_error("Not an IA setter");
    Reader r(bytes);
    r.skip(8);
    IaBinding b;
    b.type = type;
    b.context = r.read<Id>();
    if (type == 0x34f0) {
        b.start = r.read<uint32_t>();
        auto count = r.read<uint32_t>();
        if (b.start >= 32 || count > 32 - b.start)
            throw std::runtime_error("IA vertex range exceeds 32 slots");
        auto array = [&]<class T>(std::vector<T> &values) {
            if (!r.flag() && count)
                throw std::runtime_error("Nonempty IA binding requires explicit arrays");
            for (uint32_t i = 0; i < count; ++i)
                values.push_back(r.read<T>());
        };
        array(b.buffers);
        array(b.strides);
        array(b.offsets);
    } else {
        b.resource = r.read<Id>();
        if (type == 0x34f1) {
            b.format = r.read<uint32_t>();
            b.offset = r.read<uint32_t>();
        }
    }
    r.end();
    return b;
}
std::vector<uint8_t> encodeIaSetter(const IaBinding &b, Bytes prefix) {
    if (prefix.size() != 16)
        throw std::runtime_error("IA setter prefix size");
    std::vector<uint8_t> out(prefix.begin(), prefix.end());
    auto append = [&]<class T>(T value) {
        auto p = reinterpret_cast<const uint8_t *>(&value);
        out.insert(out.end(), p, p + sizeof value);
    };
    if (b.type == 0x34f0) {
        append(b.start);
        append(uint32_t(b.buffers.size()));
        auto array = [&](const auto &values) {
            append(uint8_t(1));
            for (auto value : values)
                append(value);
        };
        array(b.buffers);
        array(b.strides);
        array(b.offsets);
    } else {
        append(b.resource);
        if (b.type == 0x34f1) {
            append(b.format);
            append(b.offset);
        }
    }
    return out;
}
void validateIaBinding(const Frame &frame, const IaBinding &b) {
    requireImmediateContext(frame, b.context);
    auto resource = [&](Id id, uint16_t type, uint32_t flag = 0) {
        if (!id)
            return;
        const auto &e = frame.entry(id);
        if (e.category != 5 || e.type != type)
            throw std::runtime_error("IA binding resource type mismatch");
        if (flag && !(frame.resource(id).desc.at(2) & flag))
            throw std::runtime_error("Buffer lacks required IA bind flag");
    };
    if (b.type == 0x34ef) {
        resource(b.resource, 0x82);
        if (b.resource) {
            Reader r(frame.payload(b.resource));
            r.skip(16);
            Reader data(frame.payload(r.read<Id>(), 9, 0x84));
            auto count = data.read<uint32_t>();
            if (count > 32)
                throw std::runtime_error("Input layout element limit");
            for (uint32_t i = 0; i < count; ++i) {
                frame.data(data.read<Id>());
                data.skip(24);
            }
            data.skip(data.read<uint32_t>());
            data.end();
        }
    } else if (b.type == 0x34f1) {
        resource(b.resource, 0x83, 2);
        if (b.format != 42 && b.format != 57 && (b.resource || b.format))
            throw std::runtime_error("IA index format must be R16_UINT or R32_UINT");
    } else if (b.type == 0x34f0) {
        if (b.start >= 32 || b.buffers.size() > 32 - b.start || b.strides.size() != b.buffers.size() ||
            b.offsets.size() != b.buffers.size())
            throw std::runtime_error("IA vertex array range or length mismatch");
        for (size_t i = 0; i < b.buffers.size(); ++i) {
            resource(b.buffers[i], 0x83, 1);
            if (b.buffers[i] && b.strides[i] > 2048)
                throw std::runtime_error("IA vertex stride exceeds 2048 bytes");
        }
    } else
        throw std::runtime_error("Not an IA setter");
}
std::optional<Id> effectiveIaBuffer(const Frame &frame, Id id, const SrvOutputs &outputs, bool unknown) {
    if (!id)
        return Id(0);
    auto flags = frame.resource(id).desc.at(2);
    bool uncertain = false;
    for (unsigned i = 0; i < outputs.size(); ++i) {
        uint32_t flag = i < 8 ? 32 : i < 136 ? 128 : i < 140 ? 16 : 0;
        auto value = outputs[i];
        if (!(flags & flag) || value == Id(0))
            continue;
        if (!value || !frame.entries().contains(*value)) {
            uncertain = true;
            continue;
        }
        Id owner = *value;
        if (flag != 16) {
            Reader r(frame.payload(owner));
            r.skip(16);
            owner = r.read<Id>();
        }
        if (owner == id)
            return Id(0);
    }
    if (uncertain) {
        if (unknown)
            return {};
        throw std::runtime_error(
            "IA input/output conflict cannot be determined from preceding output observations");
    }
    return id;
}
IaHistory::IaHistory(const Frame &frame, Id context)
    : frame_(frame), context_(context), next_(frame.entries().begin()), outputs_(frame, context) {}
const IaObservation &IaHistory::advance(Id event, bool after) {
    while (next_ != frame_.entries().end()) {
        const auto &e = next_->second;
        if (e.id > event || (e.id == event && !after))
            break;
        ++next_;
        if (e.category != 7 ||
            (!isDraw(e.type) && e.type != 0x242 && !isIaSetter(e.type) && !isSrvOutputCommand(e.type)))
            continue;
        Reader r(frame_.payload(e.id));
        r.skip(isDraw(e.type) ? 16 : 8);
        if (r.read<Id>() != context_)
            continue;
        if (isDraw(e.type)) {
            auto s = frame_.state(frame_.event(e.id).state);
            for (unsigned i = 0; i < 32; ++i)
                state_.vertices[i] = {s.vb[i], s.strides[i], s.offsets[i]};
        } else if (e.type == 0x242) {
            for (auto &v : state_.vertices)
                v = {Id(0), Id(0), Id(0)};
        } else if (e.type == 0x34f0) {
            auto b = readIaSetter(e.type, frame_.payload(e.id));
            const auto &outputs = outputs_.advance(e.id).outputs;
            for (unsigned i = 0; i < b.buffers.size(); ++i)
                state_.vertices[b.start + i] = {effectiveIaBuffer(frame_, b.buffers[i], outputs, true),
                                                b.strides[i], b.offsets[i]};
        } else if (isSrvOutputCommand(e.type)) {
            const auto &outputs = outputs_.advance(e.id, true).outputs;
            for (auto &v : state_.vertices)
                if (v[0]) {
                    auto id = effectiveIaBuffer(frame_, *v[0], outputs, true);
                    if (*v[0] && id == Id(0))
                        v = {Id(0), Id(0), Id(0)};
                    else
                        v[0] = id;
                }
        }
    }
    state_.outputs = outputs_.advance(event, after).outputs;
    return state_;
}
void IaBindings::clear() {
    layout.reset();
    index.reset();
    vertices.clear();
}
void IaBindings::transition(const IaBinding &original, const IaBinding *edited,
                            const IaObservation &previous) {
    if (original.type == 0x34ef) {
        layout = edited ? std::optional<Id>(edited->resource) : std::nullopt;
        return;
    }
    if (original.type == 0x34f1) {
        index = edited ? std::optional<IaTuple>({edited->resource, edited->format, edited->offset})
                       : std::nullopt;
        return;
    }
    auto next = vertices;
    for (unsigned i = original.start; i < original.start + original.buffers.size(); ++i) {
        if (!edited)
            next.erase(i);
        else if ((i < edited->start || i >= edited->start + edited->buffers.size()) && !next.contains(i)) {
            auto v = previous.vertices[i];
            if (!v[0] || !v[1] || !v[2])
                throw std::runtime_error(
                    "Moved IA range requires earlier buffer, stride and offset observations");
            next[i] = {*v[0], *v[1], *v[2]};
        }
    }
    if (edited)
        for (unsigned i = 0; i < edited->buffers.size(); ++i)
            next[edited->start + i] = {edited->buffers[i], edited->strides[i], edited->offsets[i]};
    vertices = std::move(next);
}
void IaBindings::hazards(const Frame &frame, const SrvOutputs &outputs, bool after) {
    auto apply = [&](IaTuple &v) {
        auto actual = *effectiveIaBuffer(frame, v[0], outputs);
        if (after && v[0] && !actual)
            v = {0, 0, 0};
        else
            v[0] = actual;
    };
    for (auto &[slot, v] : vertices)
        apply(v);
    if (index)
        apply(*index);
}
void IaBindings::apply(State &s, bool includeBuffers) const {
    if (layout)
        s.layout = *layout;
    if (!includeBuffers)
        return;
    if (index) {
        s.ib = (*index)[0];
        s.ibFormat = uint32_t((*index)[1]);
        s.ibOffset = uint32_t((*index)[2]);
    }
    for (const auto &[i, v] : vertices) {
        s.vb[i] = v[0];
        s.strides[i] = uint32_t(v[1]);
        s.offsets[i] = uint32_t(v[2]);
    }
}
State effectiveIaBindings(const Frame &frame, Id event, State state, const std::map<Id, IaBinding> &edits,
                          bool includeBuffers) {
    if (edits.empty())
        return state;
    IaBindings tracker;
    std::map<Id, std::unique_ptr<IaHistory>> histories;
    auto history = [&](Id context, Id id, bool after = false) -> const IaObservation & {
        auto &p = histories[context];
        if (!p)
            p = std::make_unique<IaHistory>(frame, context);
        return p->advance(id, after);
    };
    for (const auto &[id, e] : frame.entries()) {
        if (id >= event)
            break;
        if (e.category != 7)
            continue;
        if (e.type == 0x242)
            tracker.clear();
        else if (isIaSetter(e.type) && (includeBuffers || e.type == 0x34ef)) {
            auto edit = edits.find(id);
            if (edit == edits.end() && !tracker.active())
                continue;
            auto original = readIaSetter(e.type, frame.payload(id));
            if (edit == edits.end() && e.type == 0x34ef && original.resource &&
                !frame.entries().contains(original.resource)) {
                tracker.layout.reset();
                continue;
            }
            validateIaBinding(frame, edit == edits.end() ? original : edit->second);
            tracker.transition(original, edit == edits.end() ? nullptr : &edit->second,
                               e.type == 0x34ef ? IaObservation{} : history(original.context, id));
            if (e.type != 0x34ef)
                tracker.hazards(frame, history(original.context, id).outputs);
        } else if (includeBuffers && (tracker.index || !tracker.vertices.empty()) && isDraw(e.type))
            tracker.hazards(frame, srvOutputs(frame.state(frame.event(id).state)));
        else if (includeBuffers && (tracker.index || !tracker.vertices.empty()) &&
                 isSrvOutputCommand(e.type)) {
            Reader r(frame.payload(id));
            r.skip(8);
            tracker.hazards(frame, history(r.read<Id>(), id, true).outputs, true);
        }
    }
    if (includeBuffers)
        tracker.hazards(frame, srvOutputs(state));
    tracker.apply(state, includeBuffers);
    return state;
}
} // namespace flora
