#include "BufferBindings.h"
#include <algorithm>

namespace flora {
std::vector<BufferBinding> bufferBindings(const Frame &frame, const Event &event, const State &state,
                                          Id resource) {
    if (!resource)
        throw std::runtime_error("Buffer resource ID must be nonzero");
    bool compute = event.type == 0x35 || event.type == 0x36;
    std::vector<BufferBinding> out;
    if (!compute) {
        for (int i = 0; i < 32; ++i)
            if (state.vb[i] == resource)
                out.push_back({"vb", "", i});
        if (state.soCount > state.so.size())
            throw std::runtime_error("Stream-output slot count invalid");
        for (uint32_t i = 0; i < state.soCount; ++i)
            if (state.so[i] == resource)
                out.push_back({"so", "so", int(i), 0, state.soOffsets[i]});
        if (state.ib == resource)
            out.push_back({"ib"});
    }
    if (event.argumentBuffer == resource)
        out.push_back({"indirect"});
    auto targets = [&](Id view) {
        if (!view)
            return false;
        Reader r(frame.payload(view, 5));
        r.skip(16);
        return r.read<Id>() == resource;
    };
    static constexpr const char *stages[]{"vs", "hs", "ds", "gs", "ps", "cs"};
    for (int i = compute ? 5 : 0; i < (compute ? 6 : 5); ++i) {
        const auto &stage = state.stages[i];
        for (int slot = 0; slot < 14; ++slot)
            if (stage.cb[slot] == resource)
                out.push_back({"cb", stages[i], slot});
        for (int slot = 0; slot < 128; ++slot)
            if (targets(stage.srv[slot]))
                out.push_back({"srv", stages[i], slot, stage.srv[slot]});
    }
    auto count = compute ? 64u : state.rtCount;
    if (count > 64)
        throw std::runtime_error("Output slot count invalid");
    for (uint32_t slot = 0; slot < count; ++slot) {
        auto view = compute ? (slot < 8 ? state.csUav[slot] : state.csExtended[slot - 8])
                            : (slot < 8 ? state.rtv[slot] : state.omExtended[slot - 8]);
        if (targets(view))
            out.push_back(
                {compute || slot >= state.omStart ? "uav" : "rtv", compute ? "cs" : "om", int(slot), view});
    }
    return out;
}
bool persistentBufferEdit(const std::vector<BufferBinding> &bindings) {
    return std::any_of(bindings.begin(), bindings.end(), [](const BufferBinding &b) {
        return b.role == "uav" || b.role == "rtv" || b.role == "so";
    });
}
void validateBufferPatch(const Frame &frame, Id event, Id resource, uint64_t offset, size_t size,
                         const State *effective) {
    auto command = frame.event(event);
    auto target = frame.resource(resource);
    if (target.type != 0x83)
        throw std::runtime_error("Buffer edit target is not a buffer");
    auto width = target.desc.at(0);
    if (!size || offset > width || size > width - offset)
        throw std::runtime_error("Buffer edit is empty or out of bounds");
    if (bufferBindings(frame, command, effective ? *effective : frame.state(command.state), resource).empty())
        throw std::runtime_error("Buffer is not bound to the selected draw or dispatch");
}
} // namespace flora
