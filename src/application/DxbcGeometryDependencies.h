#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
// Sufficient conditional proof: no live atomic result can affect shader outputs.
// The caller must exclude written UAV owners from every reported SRV/UAV read
// and IA/CB input. A static proof alone does not establish runtime independence.
// An unproved shader must retain the conservative instance-boundary gate.
nlohmann::json geometryUavIndependence(Bytes bytecode);
} // namespace flora
