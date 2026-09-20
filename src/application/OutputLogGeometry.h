#pragma once
#include "PostTransform.h"
namespace flora {
bool isOutputLogStage(const std::string &stage);
PostTransformGeometry outputLogGeometry(const nlohmann::json &event, nlohmann::json metadata,
                                        std::vector<uint8_t> records);
nlohmann::json outputLogTables(const PostTransformGeometry &geometry);
void exportOutputLog(const PostTransformGeometry &geometry, const std::filesystem::path &directory);
PostTransformGeometry hullGeometry(const nlohmann::json &event, Id shader, nlohmann::json metadata,
                                   std::vector<uint8_t> bytes, nlohmann::json downstream);
nlohmann::json hullTables(const PostTransformGeometry &geometry);
void exportHull(const PostTransformGeometry &geometry, const std::filesystem::path &directory);
} // namespace flora
