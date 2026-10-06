#include "ClassCreation.h"
namespace flora {
bool isClassCreation(uint16_t t) { return t == 0x3588 || t == 0x3195 || t == 0x3196; }
ClassCreationRecord readClassCreation(uint16_t t, Bytes b) {
    if (!isClassCreation(t))
        throw std::runtime_error("Unknown class creation record");
    ClassCreationRecord c;
    c.type = t;
    Reader r(b);
    c.link = r.read<Id>();
    c.owner = r.read<Id>();
    c.result = r.read<int32_t>();
    if (t == 0x3195)
        c.arguments[0] = r.read<uint32_t>();
    if (t == 0x3196)
        c.arguments = r.array<uint32_t, 4>();
    c.resource = r.read<Id>();
    r.end();
    return c;
}
ClassCreationAudit auditClassCreations(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    ClassCreationAudit a;
    a.identities = auditClassIdentities(frame, cancelled);
    for (const auto &[id, e] : frame.entries()) {
        checkCancellation(cancelled);
        if (e.category == 7 && isClassCreation(e.type)) {
            auto &c = a.records[id];
            try {
                c = readClassCreation(e.type, frame.payload(id));
                if (c.link)
                    throw std::runtime_error("Nested class creation is unresolved");
                if (c.result) {
                    if (c.result > 1 || c.resource)
                        throw std::runtime_error(
                            "Noncreating class result has unexpected status or identity");
                    continue;
                }
                if (!c.resource)
                    throw std::runtime_error("Successful class creation has no identity");
                c.canonicalOwner = e.type == 0x3588 ? c.owner : canonicalClassLinkage(a.identities, c.owner);
                c.canonicalResource =
                    e.type == 0x3588 ? canonicalClassLinkage(a.identities, c.resource) : c.resource;
                frame.payload(c.canonicalOwner, 5, e.type == 0x3588 ? 0x81 : 0x97);
                if (e.type != 0x3588) {
                    Reader owner(frame.payload(c.canonicalOwner, 5, 0x97));
                    owner.skip(8);
                    frame.payload(owner.read<Id>(), 5, 0x81);
                    owner.end();
                }
                if (!a.creationEvents.emplace(c.canonicalResource, id).second)
                    throw std::runtime_error("Repeated class identity needs version/lifetime evidence");
                if (!frame.entries().contains(c.canonicalResource)) {
                    if (e.type != 0x3588)
                        c.note = "Class instance name snapshot is absent; retain unused creation as metadata "
                                 "and reject required use";
                    continue;
                }
                c.saved = readClassRecord(frame, c.canonicalResource);
                if (e.type == 0x3588) {
                    if (c.saved.instance)
                        throw std::runtime_error("Linkage creation returned an instance snapshot");
                    Reader r(frame.payload(c.canonicalResource, 5, 0x97));
                    r.skip(8);
                    if (r.read<Id>() != c.owner)
                        throw std::runtime_error("Linkage snapshot device differs from creation");
                } else {
                    if (!c.saved.instance || c.saved.linkage != c.canonicalOwner)
                        throw std::runtime_error("Instance linkage differs from creation owner");
                    const auto &d = c.saved.desc;
                    if (bool(d[7]) != (e.type == 0x3196))
                        throw std::runtime_error("Instance creation method differs from snapshot");
                    if (e.type == 0x3195) {
                        if (c.arguments[0] != d[1])
                            throw std::runtime_error("Instance index differs from snapshot");
                    } else
                        for (unsigned i = 0; i < 4; i++)
                            if (c.arguments[i] != d[i + 3])
                                throw std::runtime_error("Instance offsets differ from snapshot");
                }
            } catch (const std::exception &error) {
                c.error = error.what();
            }
        }
    }
    for (auto &[event, c] : a.records) {
        checkCancellation(cancelled);
        if (c.error.empty() && !c.note.empty()) {
            try {
                for (const auto &[id, e] : frame.entries()) {
                    checkCancellation(cancelled);
                    if (e.category == 3 && e.type == 3) {
                        const auto state = frame.state(id);
                        for (const auto &s : state.stages) {
                            if (s.classCount > s.classes.size())
                                throw std::runtime_error("Invalid class count in state snapshot");
                            for (unsigned i = 0; i < s.classCount; i++)
                                if (s.classes[i] == c.resource)
                                    throw std::runtime_error(
                                        "Missing class instance name is required by state snapshot " +
                                        std::to_string(id));
                        }
                    }
                }
            } catch (const OperationCancelled &) {
                throw;
            } catch (const std::exception &error) {
                c.error = error.what();
            }
        }
    }
    return a;
}
const ClassCreationRecord &requireClassCreation(const ClassCreationAudit &a, Id id) {
    const auto &c = a.records.at(id);
    if (!c.error.empty())
        throw std::runtime_error("Class creation event " + std::to_string(id) + ", resource " +
                                 std::to_string(c.resource) + ": " + c.error);
    return c;
}
} // namespace flora
