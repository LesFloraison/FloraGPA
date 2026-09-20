#pragma once
#include "Frame.h"
namespace flora {
// Validate recovered read-only records; their captured returns do not set replay state.
// Unknown record types return false and must be handled or rejected by the caller.
bool acceptInspectionRecord(uint16_t type, Bytes payload);
} // namespace flora
