#pragma once
#include <functional>
#include <nlohmann/json.hpp>
namespace flora {
class RdcReplaySession;
using RdcJobRunner = std::function<int(const nlohmann::json &, RdcReplaySession *, const std::function<bool()> &)>;
int serveRdc(const RdcJobRunner &run);
}
