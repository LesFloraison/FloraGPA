#pragma once
#include "replay/Replay.h"
#include <nlohmann/json.hpp>
namespace flora {
const std::vector<std::string> &srvFields(unsigned dimension);
const char *srvDimensionName(unsigned dimension);
unsigned srvStage(const std::string &name);
unsigned srvSlot(const nlohmann::json &value);
nlohmann::json normalizeSrv(const nlohmann::json &values);
nlohmann::json mergeSrv(nlohmann::json previous, const nlohmann::json &patch);
nlohmann::json capturedSrv(const Frame &frame, Id view);
nlohmann::json effectiveSrv(const Frame &frame, Id event, unsigned stage, unsigned slot,
                            const nlohmann::json &patch, const std::map<Id, SrvBinding> &setters = {});
D3D11_SHADER_RESOURCE_VIEW_DESC nativeSrv(const nlohmann::json &values);
} // namespace flora
