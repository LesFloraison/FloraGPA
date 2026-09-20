#pragma once
#include "core/StreamOutput.h"
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
nlohmann::json streamOutputJson(const StreamOutputDeclaration &declaration);
nlohmann::json drawAutoJson(const DrawAutoParameters &parameters);
} // namespace flora
