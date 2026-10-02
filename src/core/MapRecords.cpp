#include "MapRecords.h"
#include "Contexts.h"
#include <tuple>
namespace flora {
bool isMapObservation(uint16_t type) { return type == 0x34ec || type == 0x34ed; }
MapRecordAudit auditMapRecords(const Frame &frame) {
    MapRecordAudit out;
    using Key = std::tuple<Id, Id, uint32_t>;
    std::map<Key, Id> pending;
    for (const auto &[id, entry] : frame.entries()) {
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
    for (const auto &[key, id] : pending)
        if (frame.entry(id).type == 0x34ec && out.at(id).error.empty())
            out.at(id).error = "Successful READ Map has no matching captured Unmap";
    return out;
}
const MapRecordEvidence &requireMapRecord(const MapRecordAudit &audit, Id event) {
    const auto &record = audit.at(event);
    if (!record.error.empty())
        throw std::runtime_error("Map/Unmap event " + std::to_string(event) + ", resource " +
                                 std::to_string(record.resource) + ": " + record.error);
    return record;
}
} // namespace flora
