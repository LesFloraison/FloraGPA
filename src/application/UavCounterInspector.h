#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json inspectUavCounters(const Frame &frame, Replay &replay, Id event, Id resource);
}
