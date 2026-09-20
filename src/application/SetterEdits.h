#pragma once
#include "core/PipelineBindings.h"
#include "core/Predication.h"
#include "core/SamplerBindings.h"
#include "core/SrvBindings.h"
#include <nlohmann/json.hpp>
namespace flora {
bool isEditableSetter(uint16_t type);
nlohmann::json capturedSetter(const Frame &frame, Id event);
PredicateBinding validatePredicateSetter(const Frame &frame, Id event, const nlohmann::json &values);
SamplerBinding validateSamplerSetter(const Frame &frame, Id event, const nlohmann::json &values);
SrvBinding validateSrvSetter(const Frame &frame, Id event, const nlohmann::json &values);
nlohmann::json pipelineSetterValues(const PipelineBinding &binding);
PipelineBinding validatePipelineSetter(const Frame &frame, Id event, const nlohmann::json &values);
} // namespace flora
