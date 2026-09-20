#pragma once
#include "DepthStencilEdits.h"
namespace flora {
nlohmann::json normalizePipeline(const nlohmann::json &values);
void mergePipeline(nlohmann::json &base, const nlohmann::json &normalized);
nlohmann::json depthPipelineFields(const nlohmann::json &normalized);
nlohmann::json capturedRasterizer(const Frame &frame, Id event);
nlohmann::json effectiveRasterizer(const Frame &frame, Id event, const nlohmann::json &normalized);
ReplayOptions::RasterizerEdit rasterizerEdit(const Frame &frame, Id event, const nlohmann::json &normalized);
} // namespace flora
