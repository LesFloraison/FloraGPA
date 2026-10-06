#include "BufferCreation.h"
namespace flora {
BufferCreationAudit auditBufferCreations(const Frame &frame, const CancelCheck &cancelled) {
    checkCancellation(cancelled);
    BufferCreationAudit audit;
    for (const auto &[id, entry] : frame.entries()) {
        checkCancellation(cancelled);
        if (entry.category != 7 || entry.type != 0x3578)
            continue;
        auto &out = audit.records[id];
        try {
            Reader r(frame.payload(id));
            const auto link = r.read<Id>();
            out.device = r.read<Id>();
            out.result = r.read<int32_t>();
            out.hasDescriptor = r.flag();
            if (out.hasDescriptor)
                out.descriptor = r.array<uint32_t, 6>();
            out.hasInitial = r.flag();
            if (out.hasInitial) {
                out.pointer = r.read<Id>();
                r.skip(8);
            }
            out.resource = r.read<Id>();
            r.end();
            if (link)
                throw std::runtime_error("Linked CreateBuffer execution is unresolved");
            if (out.result != 0) {
                if (out.result > 1 || out.resource)
                    throw std::runtime_error("Unexpected CreateBuffer result or returned identity");
                // Failed calls and validation-only S_FALSE create no replay resource.
                continue;
            }
            if (!out.hasDescriptor || !out.resource || !out.descriptor[0])
                throw std::runtime_error("Successful CreateBuffer lacks a descriptor, size or identity");
            if (out.descriptor[1] > 3 || (out.descriptor[1] == 1 && !out.hasInitial))
                throw std::runtime_error("CreateBuffer usage or immutable initialization is invalid");
            auto [it, inserted] = audit.creationEvents.emplace(out.resource, id);
            if (!inserted) {
                audit.records.at(it->second).error =
                    "Repeated CreateBuffer identity requires unresolved resource versioning";
                throw std::runtime_error(
                    "Repeated CreateBuffer identity requires unresolved resource versioning");
            }
            frame.payload(out.device, 5, 0x81);
            const auto resource = frame.resource(out.resource);
            if (resource.type != 0x83 || resource.device != out.device ||
                resource.desc != std::vector<uint32_t>(out.descriptor.begin(), out.descriptor.end()))
                throw std::runtime_error(
                    "CreateBuffer parameters disagree with the captured buffer resource");
            if (out.hasInitial) {
                if (!out.pointer || !resource.data)
                    throw std::runtime_error("Captured CreateBuffer initial bytes are missing; process "
                                             "pointer is not replay data");
                if (frame.data(resource.data).size() != out.descriptor[0])
                    throw std::runtime_error("CreateBuffer initial byte length differs from ByteWidth");
                out.data = resource.data;
            }
            // No-initial-data calls must NOT consume the resource's later first-use snapshot.
        } catch (const std::exception &error) {
            out.error = error.what();
        }
    }
    return audit;
}
const BufferCreationRecord &requireBufferCreation(const BufferCreationAudit &audit, Id event) {
    const auto &record = audit.records.at(event);
    if (!record.error.empty())
        throw std::runtime_error("CreateBuffer event " + std::to_string(event) + ", resource " +
                                 std::to_string(record.resource) + ": " + record.error);
    return record;
}
PrivateDataObservation readPrivateDataObservation(Bytes payload) {
    Reader r(payload);
    r.skip(8);
    PrivateDataObservation out;
    out.owner = r.read<Id>();
    out.result = r.read<int32_t>();
    r.skip(16);
    out.size = r.read<uint32_t>();
    out.pointer = r.read<Id>();
    r.end();
    if (out.result >= 0 && !out.pointer && out.size)
        throw std::runtime_error("Successful SetPrivateData has size without a captured pointer");
    return out;
}
} // namespace flora
