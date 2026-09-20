#pragma once
#include "Experiment.h"
namespace flora {
nlohmann::json normalizeBlend(const nlohmann::json &values);
void mergeBlend(nlohmann::json &base, const nlohmann::json &patch);
nlohmann::json capturedBlend(const Frame &frame, Id event, const State *base = nullptr);
nlohmann::json effectiveBlend(const Frame &frame, Id event, const nlohmann::json &normalized,
                              const State *base = nullptr);
ReplayOptions::BlendEdit blendEdit(const Frame &frame, Id event, const nlohmann::json &normalized,
                                   const State *base = nullptr);
} // namespace flora
