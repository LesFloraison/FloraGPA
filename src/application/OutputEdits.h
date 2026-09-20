#pragma once
#include "core/OutputBindings.h"
#include <nlohmann/json.hpp>
namespace flora {
// Kept separate from isEditableSetter until runtime/Qt integration is complete.
nlohmann::json capturedOutputSetter(const Frame &frame, Id event);
std::vector<uint8_t> validateOutputSetter(const Frame &frame, Id event, const nlohmann::json &values);
} // namespace flora
