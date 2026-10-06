#pragma once
#include "Frame.h"
namespace flora {
struct BufferCreationRecord {
    Id device{}, resource{}, pointer{}, data{};
    int32_t result{};
    bool hasDescriptor{}, hasInitial{};
    std::array<uint32_t, 6> descriptor{};
    std::string error;
};
struct BufferCreationAudit {
    std::map<Id, BufferCreationRecord> records;
    std::map<Id, Id> creationEvents;
};
BufferCreationAudit auditBufferCreations(const Frame &frame, const CancelCheck &cancelled = {});
const BufferCreationRecord &requireBufferCreation(const BufferCreationAudit &audit, Id event);
struct PrivateDataObservation {
    Id owner{}, pointer{};
    uint32_t size{};
    int32_t result{};
};
PrivateDataObservation readPrivateDataObservation(Bytes payload);
} // namespace flora
