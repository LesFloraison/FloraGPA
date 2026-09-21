#pragma once
#include <nlohmann/json.hpp>
namespace flora {
// One job per isolated process. RenderDoc replay and capture must not share a process.
nlohmann::json runRdcJob(const nlohmann::json &job);
} // namespace flora
