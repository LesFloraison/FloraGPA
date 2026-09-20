#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json normalizeDepthStencil(const nlohmann::json &values);
void mergeDepthStencil(nlohmann::json &base, const nlohmann::json &normalized);
nlohmann::json capturedDepthStencil(const Frame &frame, Id event, const State *base = nullptr);
nlohmann::json effectiveDepthStencil(const Frame &frame, Id event, const nlohmann::json &normalized,
                                     const State *base = nullptr);
ReplayOptions::DepthStencilEdit
depthStencilEdit(const Frame &frame, Id event, const nlohmann::json &normalized, const State *base = nullptr);
} // namespace flora
