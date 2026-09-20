#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json inspectShader(Bytes bytecode);
nlohmann::json inspectResourceShader(const Frame &frame, Id resource, Bytes bytecode);
}
