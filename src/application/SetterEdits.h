#pragma once
#include "core/Predication.h"
#include "core/SamplerBindings.h"
#include <nlohmann/json.hpp>
namespace flora {
bool isEditableSetter(uint16_t type);
nlohmann::json capturedSetter(const Frame &frame, Id event);
PredicateBinding validatePredicateSetter(const Frame &frame, Id event, const nlohmann::json &values);
SamplerBinding validateSamplerSetter(const Frame &frame, Id event, const nlohmann::json &values);
} // namespace flora
