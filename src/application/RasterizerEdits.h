#pragma once
#include "DepthStencilEdits.h"
namespace flora {
nlohmann::json normalizePipeline(const nlohmann::json &values);
void mergePipeline(nlohmann::json &base, const nlohmann::json &normalized);
nlohmann::json depthPipelineFields(const nlohmann::json &normalized);
nlohmann::json capturedRasterizer(const Frame &frame, Id event, const State *base = nullptr);
nlohmann::json effectiveRasterizer(const Frame &frame, Id event, const nlohmann::json &normalized,
                                   const State *base = nullptr);
ReplayOptions::RasterizerEdit rasterizerEdit(const Frame &frame, Id event, const nlohmann::json &normalized,
                                             const State *base = nullptr);
} // namespace flora
