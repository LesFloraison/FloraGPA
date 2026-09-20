#pragma once
#include "Experiment.h"
namespace flora {
nlohmann::json normalizeBlend(const nlohmann::json &values);
void mergeBlend(nlohmann::json &base, const nlohmann::json &patch);
nlohmann::json capturedBlend(const Frame &frame, Id event);
nlohmann::json effectiveBlend(const Frame &frame, Id event, const nlohmann::json &normalized);
ReplayOptions::BlendEdit blendEdit(const Frame &frame, Id event, const nlohmann::json &normalized);
} // namespace flora
