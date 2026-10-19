#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
#include <map>
namespace flora {
nlohmann::json rdcCounterDescription(const CounterDescription &description);
nlohmann::json rdcCounterValue(const CounterDescription &description, const CounterResult &result);
std::string rdcCounterUnit(CounterUnit unit);
rdcarray<GPUCounter> selectRdcCounters(const std::map<GPUCounter, CounterDescription> &available,
                                     const nlohmann::json &requested);
} // namespace flora
