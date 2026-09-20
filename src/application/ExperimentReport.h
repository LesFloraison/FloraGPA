#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json experimentReport(const Replay &replay);
} // namespace flora
