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
struct MappedWriteLayout {
    bool texture{}, full{};
    uint32_t row{}, rows{}, depth{1}, planarFormat{};
    uint64_t sourceRowPitch{};
    std::vector<std::pair<size_t, Bytes>> updates;
    Bytes tight;
    std::vector<uint8_t> recoveredLuma;
};
// Call after requireMapRecord. Failed calls carry no storage effect. Byte spans
// refer to the immutable Frame; recovered planar Y bytes are owned by the result.
MappedWriteLayout mappedWriteLayout(const Frame &frame, const MapRecordEvidence &record);
} // namespace flora
