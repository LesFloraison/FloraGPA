#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json rdcCounterDescription(const CounterDescription &description);
nlohmann::json rdcCounterValue(const CounterDescription &description, const CounterResult &result);
std::string rdcCounterUnit(CounterUnit unit);
} // namespace flora
