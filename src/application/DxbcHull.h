#pragma once
#include "DxbcOutputLog.h"
namespace flora {
struct HullInstanceShaders {
    std::vector<uint8_t> vertex, hull;
    nlohmann::json identity;
};
nlohmann::json hullOutputSchema(Bytes original);
HullInstanceShaders carryHullInstance(Bytes vertex, Bytes hull);
OutputLogShader instrumentHullOutputs(Bytes original, uint32_t slot, uint32_t patches,
                                      const nlohmann::json &identity = nullptr,
                                      uint32_t patchesPerInstance = 0);
} // namespace flora
