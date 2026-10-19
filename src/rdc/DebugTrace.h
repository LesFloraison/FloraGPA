#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>

namespace flora {
// Preserve the public replay API's full typed values, including aliases of the
// same raw storage. Non-finite floats use the reference worker's JSON strings.
nlohmann::json rdcVariable(const ShaderVariable &value);
nlohmann::json rdcDebugTrace(const ShaderDebugTrace &trace);
nlohmann::json rdcDebugState(const ShaderDebugState &state);
nlohmann::json rdcDebugInfo(const ShaderDebugInfo &info);
// Compare only reflected, mapped target-0 components against an eligible GPU
// fragment reference. Exact output equality does not validate the whole trace.
nlohmann::json rdcComparePixelOutput(const ShaderReflection &reflection,
                                    const nlohmann::json &trace,
                                    const nlohmann::json &steps,
                                    const nlohmann::json &reference);
} // namespace flora
