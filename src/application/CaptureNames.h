#pragma once
#include "core/Frame.h"
#include <QString>
#include <nlohmann/json.hpp>
namespace flora {
QString debugDisplayName(Bytes bytes);
nlohmann::json capturedNames(const Frame &frame);
} // namespace flora
