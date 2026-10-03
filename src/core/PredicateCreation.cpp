#include "PredicateCreation.h"
namespace flora {
PredicateCreationRecord readPredicateCreation(Bytes bytes) {
    Reader r(bytes);
    PredicateCreationRecord c;
    c.link = r.read<Id>();
    c.device = r.read<Id>();
    c.result = r.read<int32_t>();
    c.hasDescriptor = r.flag();
    if (c.hasDescriptor) {
        c.type = r.read<uint32_t>();
        c.flags = r.read<uint32_t>();
    }
    c.resource = r.read<Id>();
    r.end();
    return c;
}
PredicateCreationAudit auditPredicateCreations(const Frame &frame) {
    PredicateCreationAudit a;
    for (const auto &[id, e] : frame.entries()) {
        if (e.category != 7 || e.type != 0x358e)
            continue;
        auto &c = a.records[id];
        try {
            c = readPredicateCreation(frame.payload(id));
            if (c.link)
                throw std::runtime_error("Nested predicate creation is unresolved");
            if (c.result) {
                if (c.result > 1 || c.resource)
                    throw std::runtime_error(
                        "Noncreating predicate result has unexpected status or identity");
                continue;
            }
            if (!c.hasDescriptor || !c.resource)
                throw std::runtime_error("Successful predicate creation lacks descriptor or identity");
            if ((c.type != 5 && c.type != 7) || (c.flags & ~1u) || (c.flags && c.type != 5))
                throw std::runtime_error("Unsupported predicate creation type or flags");
            frame.payload(c.device, 5, 0x81);
            auto [it, added] = a.creationEvents.emplace(c.resource, id);
            if (!added) {
                a.records.at(it->second).error =
                    "Repeated predicate identity needs lifetime/version evidence";
                throw std::runtime_error("Repeated predicate identity needs lifetime/version evidence");
            }
            if (frame.entries().contains(c.resource)) {
                const auto saved = readPredicate(frame, c.resource);
                if (saved.device != c.device || saved.type != c.type || saved.flags != c.flags)
                    throw std::runtime_error("Predicate creation differs from saved resource descriptor");
            }
            // The inline descriptor fully specifies even an unused predicate lacking a snapshot.
        } catch (const std::exception &error) {
            c.error = error.what();
        }
    }
    return a;
}
const PredicateCreationRecord &requirePredicateCreation(const PredicateCreationAudit &a, Id event) {
    const auto &c = a.records.at(event);
    if (!c.error.empty())
        throw std::runtime_error("Predicate creation event " + std::to_string(event) + ", resource " +
                                 std::to_string(c.resource) + ": " + c.error);
    return c;
}
} // namespace flora
