#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json readResourceWorkspace(IReplayController &, const nlohmann::json &job,
                                     const nlohmann::json &queryPolicy);
}
