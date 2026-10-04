#include "ContextStateRecords.h"
#include "Contexts.h"
namespace flora {
bool isContextStateRecord(uint16_t type) { return type == 0x3561 || type == 0x35a4 || type == 0x359b; }
ContextStateRecord readContextStateRecord(uint16_t type, Bytes payload) {
    if (!isContextStateRecord(type))
        throw std::runtime_error("Not a recovered context-state record");
    Reader r(payload);
    ContextStateRecord out;
    out.type = type;
    out.link = r.read<Id>();
    out.owner = r.read<Id>();
    if (type == 0x3561) {
        out.state = r.read<Id>();
        out.previous = r.read<Id>();
    } else if (type == 0x359b) {
        out.flags = r.read<uint32_t>();
    } else {
        out.result = r.read<int32_t>();
        out.flags = r.read<uint32_t>();
        out.levelCount = r.read<uint32_t>();
        out.hasLevels = r.flag();
        if (out.hasLevels) {
            if (!out.levelCount || out.levelCount > 64 || out.levelCount > r.remaining() / 4)
                throw std::runtime_error("Invalid context-state feature-level array length");
            out.levels.reserve(out.levelCount);
            for (uint32_t i = 0; i < out.levelCount; ++i)
                out.levels.push_back(r.read<uint32_t>());
        }
        out.sdk = r.read<uint32_t>();
        out.emulatedInterface = r.array<uint8_t, 16>();
        if (r.flag())
            out.chosenLevel = r.read<uint32_t>();
        out.state = r.read<Id>();
    }
    r.end();
    return out;
}
void validateContextStateOwner(const Frame &frame, Id event, const ContextStateRecord &record) {
    if (record.type == 0x3561)
        requireImmediateContext(frame, record.owner);
    else if (!record.owner || frame.entry(record.owner).category != 5 ||
             frame.entry(record.owner).type != 0x81)
        throw std::runtime_error("Context-state device reference is not a saved DX11 device");
    if (record.link) {
        // Original CreateDeviceContextState calls GetCreationFlags internally.
        const auto &entry = frame.entry(record.link);
        if (record.type != 0x359b || record.link >= event || entry.category != 7 || entry.type != 0x35a4)
            throw std::runtime_error("Unproven linked context-state record");
        const auto creation = readContextStateRecord(entry.type, frame.payload(entry.id));
        if (creation.link || creation.owner != record.owner)
            throw std::runtime_error("Linked GetCreationFlags device does not match context-state creation");
    }
}
std::string contextStateReplayGap(const ContextStateRecord &r) {
    if (r.type == 0x3561) {
        if (!r.state)
            return "SwapDeviceContextState input identity is zero: this GPA serializer also writes zero "
                   "for unregistered non-null state objects. A null no-op cannot be proven; swapped "
                   "pipeline state and subsequent draw snapshots cannot be trusted";
        return "SwapDeviceContextState object identity, saved pipeline and restoration semantics are "
               "not implemented for this layout";
    }
    if (r.type == 0x35a4) {
        if (r.result == 0 && !r.state)
            return "CreateDeviceContextState succeeded but saved no returned object identity. The "
                   "original serializer can lose a non-null state object; creation arguments cannot "
                   "recover subsequent state identity or pipeline contents";
        return "CreateDeviceContextState lifetime and interface-emulation semantics are not implemented";
    }
    return {};
}
const char *contextStateGapKind(const ContextStateRecord &r) {
    if ((r.type == 0x3561 && !r.state) || (r.type == 0x35a4 && r.result == 0 && !r.state))
        return "context_state_identity_unresolved";
    return "implementation_gap";
}
} // namespace flora
