#include "NormalizedPredication.h"
#include "BufferCreation.h"
#include "ClassCreation.h"
#include "Contexts.h"
#include "PipelineCreation.h"
#include "PredicateCreation.h"
#include "TextureCreation.h"
#include <set>
namespace flora {
std::map<Id, NormalizedPredicateProof> auditNormalizedPredication(const Frame &frame) {
    std::map<Id, PredicateCommand> setters;
    std::map<Id, std::vector<Id>> children;
    std::set<Id> intervals, created;
    // Do not infer a condition through malformed control/identity records.
    try {
        for (const auto &[id, e] : frame.entries()) {
            if (e.category != 7)
                continue;
            if (const auto operation = predicateOperation(e.type)) {
                const auto p = readPredicateCommand(e.type, frame.payload(id));
                if (*operation != PredicateOperation::Set)
                    intervals.insert(p.resource);
                else if (e.type == 0x248 && p.resource && p.value <= 1 &&
                         !frame.entries().contains(p.resource)) {
                    Reader r(frame.payload(id));
                    if (r.read<Id>() == 0) {
                        requireImmediateContext(frame, p.context);
                        setters[id] = p;
                    }
                }
            } else if (e.type == 0x358e) {
                const auto creation = readPredicateCreation(frame.payload(id));
                if (creation.resource)
                    created.insert(creation.resource);
            } else if (e.type == 0x3166) {
                Reader r(frame.payload(id));
                children[r.read<Id>()].push_back(id);
            }
        }
        if (setters.empty())
            return {};
        // Do not reinterpret a missing snapshot ID that belongs to another
        // frame-created object (including creations later in the capture).
        auto exclude = [&](const auto &events) {
            for (const auto &[resource, event] : events)
                created.insert(resource);
        };
        exclude(auditBufferCreations(frame).creationEvents);
        exclude(auditTextureCreations(frame).creationEvents);
        exclude(auditClassCreations(frame).creationEvents);
        exclude(auditPipelineCreations(frame).creationEvents);
    } catch (const std::exception &) {
        return {};
    }
    std::map<Id, NormalizedPredicateProof> result;
    constexpr std::array<uint8_t, 16> marker{
        0x00, 0xdf, 0x60, 0x68, 0x75, 0x8a, 0xb6, 0x48,
        0xa1, 0x5e, 0xa8, 0x9f, 0x69, 0x8b, 0xf8, 0x1f};
    for (const auto &[id, p] : setters) {
        if (intervals.contains(p.resource) || created.contains(p.resource) || children[id].size() != 1)
            continue;
        const auto witness = children[id].front();
        if (witness <= id)
            continue;
        try {
            Reader r(frame.payload(witness, 7, 0x3166));
            if (r.read<Id>() != id || r.read<Id>() != p.resource ||
                r.read<uint32_t>() != 0x887a0002u || r.array<uint8_t, 16>() != marker ||
                !r.flag() || r.read<uint32_t>() != 0 || !r.read<Id>())
                continue;
            r.end();
            result[id] = {id, p.resource, witness, p.value};
        } catch (const std::exception &) {
            // An invalid or incomplete witness is not proof. The usual decoder
            // and missing-resource diagnostics remain authoritative.
        }
    }
    return result;
}
} // namespace flora
