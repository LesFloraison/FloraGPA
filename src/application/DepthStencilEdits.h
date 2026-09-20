#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json normalizeDepthStencil(const nlohmann::json &values);
void mergeDepthStencil(nlohmann::json &base, const nlohmann::json &normalized);
nlohmann::json capturedDepthStencil(const Frame &frame, Id event);
nlohmann::json effectiveDepthStencil(const Frame &frame, Id event, const nlohmann::json &normalized);
ReplayOptions::DepthStencilEdit depthStencilEdit(const Frame &frame, Id event,
                                                 const nlohmann::json &normalized);
} // namespace flora
