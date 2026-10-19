#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json readDx12Pipeline(IReplayController &controller, const ActionDescription &action);
}
