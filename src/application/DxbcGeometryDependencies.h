#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
// Default sufficient proof: no live atomic result can affect shader outputs.
// In the default mode, exclude written UAV owners from every reported SRV/UAV read
// and IA/CB input. A static proof alone does not establish runtime independence.
// An unproved shader must retain the conservative instance-boundary gate.
// Terminal DS values cannot change tessellator output counts. In that mode the
// caller must additionally prove no GS follows and exclude all writes from
// upstream count dependencies; DS resource reads themselves may alias writes.
nlohmann::json geometryUavIndependence(Bytes bytecode, bool terminalDomainCounts = false);
} // namespace flora
