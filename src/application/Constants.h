#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>

namespace flora {
nlohmann::json constantFields(const nlohmann::json &variable, Bytes data, Replay::ConstantRange range = {});
std::vector<BufferPatch> constantPatches(const nlohmann::json &field, const nlohmann::json &value);
// Recover values from their exact storage after a UI/JSON layer normalizes numeric values.
nlohmann::json constantValue(const nlohmann::json &field);
nlohmann::json inspectConstants(const Frame &frame, const Replay &replay, const ReplayOptions &options,
                                Id event, Id resource, Bytes data);
} // namespace flora
