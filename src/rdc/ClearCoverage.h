#pragma once
#include "Api.h"
#include <nlohmann/json.hpp>
namespace flora {
// A captured API coverage check, not proof that the selected value changed.
nlohmann::json rdcClearCoverage(const SDObject &chunk, const TextureDescription &texture,
                               const Subresource &sub, uint32_t x, uint32_t y);
}
