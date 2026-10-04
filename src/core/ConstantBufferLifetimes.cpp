#include "Commands.h"
#include "ConstantBufferBindings.h"
#include "InspectionRecords.h"
#include "MapRecords.h"
#include "PipelineGetters.h"
#include <algorithm>
namespace flora {
UnusedConstantBufferLifetime
proveUnusedConstantBufferLifetime(const Frame &frame, Id event, const std::set<Id> &disabled,
                                  const std::map<Id, std::vector<uint8_t>> &payloads,
                                  const std::map<Id, ConstantBufferBinding> &edits, Id until, bool before) {
    auto payload = [&](Id id) {
        const auto bytes = frame.payload(id);
        if (auto it = payloads.find(id); it != payloads.end() && !std::ranges::equal(bytes, it->second))
            throw std::runtime_error("Unused CB lifetime cannot certify changed command payloads");
        return bytes;
    };
    const auto &entry = frame.entry(event);
    if (entry.category != 7 || !constantBufferStage(entry.type) || disabled.contains(event))
        throw std::runtime_error("Unused CB lifetime requires an enabled CB setter");
    const auto initial = readConstantBufferSetter(entry.type, payload(event));
    UnusedConstantBufferLifetime proof{event, 0, *constantBufferStage(entry.type)};
    std::set<unsigned> unresolved;
    auto setter = [&](const Entry &e) {
        if (edits.contains(e.id))
            throw std::runtime_error("Unused CB lifetime cannot certify a binding edit inside the interval");
        const auto bytes = payload(e.id);
        Reader r(bytes);
        if (r.read<Id>())
            throw std::runtime_error("Unused CB lifetime has an unresolved record link");
        auto binding = readConstantBufferSetter(e.type, bytes);
        if (binding.context != initial.context)
            throw std::runtime_error("Unused CB lifetime crosses different contexts");
        const auto stage = *constantBufferStage(e.type);
        for (unsigned i = 0; i < binding.buffers.size(); ++i) {
            const auto id = binding.buffers[i];
            if (id && !frame.entries().contains(id)) {
                if (stage != proof.stage)
                    throw std::runtime_error("Unused CB lifetime crosses another unresolved shader stage");
                unresolved.insert(binding.start + i);
                if (std::find(proof.resources.begin(), proof.resources.end(), id) == proof.resources.end())
                    proof.resources.push_back(id);
                binding.buffers[i] = 0; // Validate all other fields without inventing a resource descriptor.
            } else if (stage == proof.stage)
                unresolved.erase(binding.start + i);
        }
        validateConstantBufferBinding(frame, binding);
    };
    setter(entry);
    if (unresolved.empty())
        throw std::runtime_error("Unused CB lifetime has no absent resource");
    std::optional<MapRecordAudit> maps;
    for (auto it = frame.entries().upper_bound(event); it != frame.entries().end(); ++it) {
        const auto &[id, next] = *it;
        if (next.category != 7)
            continue;
        if (until && (id > until || (before && id == until)))
            break;
        try {
            // Even disabled draws prepare their snapshots and expose native state.
            if (isDraw(next.type))
                throw std::runtime_error("GPU command precedes an explicit binding close");
            const auto raw = payload(id);
            if (disabled.contains(id))
                continue;
            if (constantBufferStage(next.type)) {
                setter(next);
                if (unresolved.empty()) {
                    proof.closingEvent = id;
                    return proof;
                }
                continue;
            }
            if (next.type == 0x242) {
                Reader r(raw);
                if (r.read<Id>() || r.read<Id>() != initial.context)
                    throw std::runtime_error("Closing ClearState has a link or different context");
                r.end();
                proof.closingEvent = id;
                return proof;
            }
            if (isPipelineGetter(next.type)) {
                const auto getter = readPipelineGetter(next.type, raw);
                validatePipelineGetter(frame, getter);
                if (getter.context != initial.context)
                    throw std::runtime_error("Getter has a different context");
                continue; // Captured observation only; not a native-state restoration.
            }
            if (next.type >= 0x3278 && next.type <= 0x327e) {
                validateAnnotationCommand(next.type, raw);
                continue;
            }
            if (next.type == 0x304b) {
                acceptPassiveObjectRecord(next.type, raw); // CommandList QI observation; no execution.
                continue;
            }
            if (next.type == 0x246 || isMapObservation(next.type)) {
                if (!maps)
                    maps = auditMapRecords(frame);
                const auto &map = requireMapRecord(*maps, id);
                if (map.context != initial.context)
                    throw std::runtime_error("Map observation has a different context");
                if (std::find(proof.resources.begin(), proof.resources.end(), map.resource) !=
                    proof.resources.end())
                    throw std::runtime_error("Map requires the absent buffer's storage");
                continue; // Saved-resource uploads/read observations do not consume CB bindings.
            }
            throw std::runtime_error("Command may consume or change unresolved CB state");
        } catch (const std::exception &error) {
            throw std::runtime_error("Unused CB lifetime blocked at event " + std::to_string(id) + ": " +
                                     error.what());
        }
    }
    throw std::runtime_error("Unused CB lifetime has no explicit close before the replay boundary");
}
} // namespace flora
