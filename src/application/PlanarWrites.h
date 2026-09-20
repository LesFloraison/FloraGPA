#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json planarWriteReport(const Replay &replay);
std::string planarWriteNotice(const Replay &replay);
} // namespace flora
