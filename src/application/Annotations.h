#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json inspectAnnotations(const Frame &frame);
void exportAnnotations(const nlohmann::json &report, const std::filesystem::path &directory);
} // namespace flora
