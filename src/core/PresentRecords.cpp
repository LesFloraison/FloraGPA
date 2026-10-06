#include "PresentRecords.h"
#include "BufferCreation.h"
#include "InspectionRecords.h"
namespace flora {
PresentRecord validatePresentRecord(const Frame &frame, Id event) {
    PresentRecord out;
    Reader r(frame.payload(event, 7, 0x3257));
    const auto parent = r.read<Id>();
    out.chain = r.read<Id>();
    const auto result = r.read<int32_t>();
    out.syncInterval = r.read<uint32_t>();
    out.flags = r.read<uint32_t>();
    r.end();
    if (parent)
        throw std::runtime_error("Linked Present requires unresolved parent execution semantics");
    out.occluded = result == 0x087a0001;
    if (result != 0 && !(out.occluded && out.flags == 1))
        throw std::runtime_error("Present HRESULT other than S_OK requires unverified status semantics");
    if (out.syncInterval > 4 || (out.flags != 0 && out.flags != 1 && out.flags != 0x200))
        throw std::runtime_error("Present sync interval or flags are outside the verified replay scope");
    out.test = out.flags == 1;
    Reader chain(frame.payload(out.chain, 5, 0x38));
    chain.skip(16);
    const auto width = chain.read<uint32_t>(), height = chain.read<uint32_t>();
    chain.skip(8);
    const auto format = chain.read<uint32_t>();
    chain.skip(8);
    const auto samples = chain.read<uint32_t>(), quality = chain.read<uint32_t>();
    chain.skip(4);
    const auto buffers = chain.read<uint32_t>();
    chain.skip(4 + 8); // ABI padding and captured HWND, never a native replay window.
    const auto windowed = chain.read<uint32_t>();
    out.swapEffect = chain.read<uint32_t>();
    const auto chainFlags = chain.read<uint32_t>();
    chain.skip(4);
    chain.end();
    const bool flip = out.swapEffect == 3 || out.swapEffect == 4;
    if (out.occluded && flip)
        throw std::runtime_error("Occluded Present TEST requires a verified blt-model swap chain");
    if (!width || !height || !samples || !buffers || windowed > 1 ||
        (out.swapEffect != 0 && out.swapEffect != 1 && !flip) ||
        (flip && (samples != 1 || quality || buffers < 2)))
        throw std::runtime_error("Unsupported captured swap-chain description for Present");
    if (out.flags == 0x200 && (!flip || !windowed || out.syncInterval || !(chainFlags & 0x800)))
        throw std::runtime_error("Captured tearing Present conflicts with swap-chain flags or interval");
    out.unbindRtv = flip && !out.test;
    if (out.unbindRtv) {
        for (const auto &[id, entry] : frame.entries()) {
            if (entry.category != 5 || entry.type != 0x85)
                continue;
            // Read the parent before decoding unrelated texture descriptions.
            Reader header(frame.payload(id));
            header.skip(8);
            if (header.read<Id>() != out.chain)
                continue;
            if (out.backbuffer)
                throw std::runtime_error("Present swap-chain backbuffer identity is ambiguous");
            const auto resource = frame.resource(id);
            const auto &d = resource.desc;
            if (d.at(0) != width || d.at(1) != height || d.at(2) != 1 || d.at(3) != 1 || d.at(4) != format ||
                d.at(5) != samples || d.at(6) != quality)
                throw std::runtime_error("Present backbuffer description conflicts with its swap chain");
            out.backbuffer = id;
        }
        if (!out.backbuffer)
            throw std::runtime_error("Present has no captured live backbuffer identity");
    }
    if (!out.test) {
        for (auto it = frame.entries().upper_bound(event); it != frame.entries().end(); ++it) {
            const auto &[id, entry] = *it;
            if (entry.category != 7)
                continue;
            auto payload = frame.payload(id);
            if (entry.type == 0x3017) {
                readPrivateDataObservation(payload);
                continue;
            }
            if (acceptPassiveObjectRecord(entry.type, payload) ||
                acceptInspectionRecord(entry.type, payload) || acceptQueryMetadata(entry.type, payload))
                continue;
            throw std::runtime_error("Post-Present execution at event " + std::to_string(id) +
                                     " requires unresolved buffer rotation/content semantics (swap chain " +
                                     std::to_string(out.chain) + ")");
        }
    }
    return out;
}
} // namespace flora
