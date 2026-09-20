#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
unsigned samplerStage(const std::string &name);
unsigned samplerSlot(const nlohmann::json &value);
nlohmann::json normalizeSampler(const nlohmann::json &values);
nlohmann::json samplerDescriptor(const Frame &frame, Id resource);
D3D11_SAMPLER_DESC nativeSampler(const nlohmann::json &values);
State samplerState(const Frame &frame, Id event, const std::map<Id, SamplerBinding> &setters);
} // namespace flora
