#pragma once
#include "core/Dxbc.h"
#include <nlohmann/json.hpp>
namespace flora {
// Sufficient proof only: no UAV result or SRV read can affect shader outputs.
// The caller must separately exclude writable-resource aliases in IA/CB inputs.
// An unproved shader must retain the conservative instance-boundary gate.
nlohmann::json geometryUavIndependence(Bytes bytecode);
} // namespace flora
