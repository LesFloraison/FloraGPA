#include "MapRecords.h"
#include "Contexts.h"
#include "TextureStorage.h"
#include <tuple>
namespace flora {
bool isMapObservation(uint16_t type) { return type == 0x34ec || type == 0x34ed; }
MapRecordAudit auditMapRecords(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    MapRecordAudit out;
    using Key = std::tuple<Id, Id, uint32_t>;
    std::map<Key, Id> pending;
    for (const auto &[id, entry] : frame.entries()) {
        checkCancellation(cancelled);
        if (entry.category != 7 || (entry.type != 0x246 && !isMapObservation(entry.type)))
            continue;
        auto &record = out[id];
        try {
            Reader r(frame.payload(id));
            auto parent = r.read<Id>();
            record.context = r.read<Id>();
            if (entry.type != 0x34ed)
                record.result = r.read<int32_t>();
            record.resource = r.read<Id>();
            record.subresource = r.read<uint32_t>();
            if (entry.type != 0x34ed) {
                record.kind = r.read<uint32_t>();
                record.flags = r.read<uint32_t>();
                record.data = r.read<Id>();
            }
            r.end();
            if (parent)
                throw std::runtime_error("Linked Map/Unmap requires unsupported parent execution semantics");
            requireImmediateContext(frame, record.context);
            Key key{record.context, record.resource, record.subresource};
            if (entry.type == 0x34ed) {
                auto previous = pending.find(key);
                if (previous == pending.end())
                    throw std::runtime_error(
                        "Unmap has no preceding successful Map for this context/resource/subresource");
                auto &map = out.at(previous->second);
                record.pairedEvent = previous->second;
                map.pairedEvent = id;
                pending.erase(previous);
                continue;
            }
            if (record.kind < 1 || record.kind > 5 || (record.flags & ~0x100000u))
                throw std::runtime_error("Invalid captured Map type or flags");
            if (record.result < 0)
                continue; // Failed calls have no mapped storage or CPU write to replay.
            if (record.result != 0)
                throw std::runtime_error("Map success status other than S_OK is unverified");
            if (record.flags && (record.kind == 4 || record.kind == 5))
                throw std::runtime_error("Successful DO_NOT_WAIT Map cannot use WRITE_DISCARD or WRITE_NO_OVERWRITE");
            if (entry.type == 0x34ec && record.kind != 1)
                throw std::runtime_error("Writable API Map cannot be skipped as read-only metadata");
            if (entry.type == 0x246 && record.kind == 1)
                throw std::runtime_error("Captured write record contains a READ Map");
            if (!record.data)
                throw std::runtime_error("Successful Map has no captured data identity");
            // A READ identity is an opaque captured pointer identity, not necessarily
            // a saved GenData record. Do not invent bytes or dereference it.
            const auto resource = frame.resource(record.resource);
            if (resource.type == 0x83) {
                if (record.subresource)
                    throw std::runtime_error("Buffer Map subresource must be zero");
            } else if (resource.type >= 0x84 && resource.type <= 0x86) {
                const auto info = textureInfo(resource);
                if (!info.mips || !info.layers || info.samples != 1 ||
                    record.subresource >= uint64_t(info.mips) * info.layers)
                    throw std::runtime_error("Map texture subresource or sample count is invalid");
            } else
                throw std::runtime_error("Map target is not a supported buffer or texture");
            if (auto previous = pending.find(key); previous != pending.end()) {
                out.at(previous->second).error = "Overlapping successful Map for the same subresource";
                throw std::runtime_error("Overlapping successful Map for the same subresource");
            }
            pending.emplace(key, id);
        } catch (const std::exception &error) {
            record.error = error.what();
        }
    }
    for (const auto &[key, id] : pending) {
        checkCancellation(cancelled);
        if (frame.entry(id).type == 0x34ec && out.at(id).error.empty())
            out.at(id).error = "Successful READ Map has no matching captured Unmap";
    }
    return out;
}
const MapRecordEvidence &requireMapRecord(const MapRecordAudit &audit, Id event) {
    const auto &record = audit.at(event);
    if (!record.error.empty())
        throw std::runtime_error("Map/Unmap event " + std::to_string(event) + ", resource " +
                                 std::to_string(record.resource) + ": " + record.error);
    return record;
}
MappedWriteLayout mappedWriteLayout(const Frame &frame, const MapRecordEvidence &record) {
    MappedWriteLayout out;
    if (record.result < 0)
        return out;
    const auto resource = frame.resource(record.resource);
    size_t size = 0;
    out.texture = resource.type != 0x83;
    out.full = frame.entry(record.data).type == 1;
    if (out.texture) {
        const auto info = textureInfo(resource);
        const auto subs = textureSubresources(resource);
        if (info.samples != 1 || record.subresource >= subs.size() || record.kind == 5)
            throw std::runtime_error("Unsupported mapped texture layout");
        const auto &sub = subs.at(record.subresource);
        out.row = sub.rowPitch;
        out.rows = pitches(sub.width, sub.height, info.format).second;
        out.depth = sub.depth;
        size = size_t(sub.size);
        if (!out.full && info.dimension != 2)
            throw std::runtime_error("Capture mapped texture pitches are unavailable");
        if (info.format >= 103 && info.format <= 105) {
            out.planarFormat = info.format;
            out.sourceRowPitch = uint64_t(info.width) * (info.format == 103 ? 1 : 3);
        }
    } else {
        if (record.subresource)
            throw std::runtime_error("Buffer subresource must be zero");
        size = resource.desc.at(0);
    }
    out.updates = frame.updates(record.data, size);
    if (out.texture && out.full) {
        out.tight = out.updates.at(0).second;
        if (out.planarFormat) {
            out.rows = textureInfo(resource).height;
            if (out.planarFormat == 103)
                out.tight = out.tight.first(size_t(out.row) * out.rows);
            else {
                auto single = resource;
                single.desc[3] = 1;
                out.recoveredLuma = capturedLuma(single, out.tight, 0, 0, 0, "y").bytes;
            }
        }
    }
    return out;
}
} // namespace flora
