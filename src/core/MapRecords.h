#pragma once
#include "Frame.h"
namespace flora {
struct MapRecordEvidence {
    Id context{}, resource{}, data{}, pairedEvent{};
    uint32_t subresource{}, kind{}, flags{};
    int32_t result{};
    std::string error;
};
// Audit captured CPU Map observations separately from 0x246 GPU write playback.
// Per-event errors allow a prefix replay without certifying later records.
using MapRecordAudit = std::map<Id, MapRecordEvidence>;
MapRecordAudit auditMapRecords(const Frame &frame);
const MapRecordEvidence &requireMapRecord(const MapRecordAudit &audit, Id event);
bool isMapObservation(uint16_t type);
} // namespace flora
