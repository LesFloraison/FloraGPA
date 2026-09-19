#pragma once
#include "core/Commands.h"
#include <nlohmann/json.hpp>

namespace flora {
nlohmann::json clearValues(uint16_t type, Bytes payload);
std::vector<uint8_t> patchClear(uint16_t type, Bytes payload, const nlohmann::json &values);
} // namespace flora
