#pragma once
#include "core/Frame.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json inspectionEvent(const Frame &frame, Id id);
}
