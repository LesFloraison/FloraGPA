#pragma once
#include <nlohmann/json.hpp>
namespace flora {
// Adapter input uses original RenderDoc ActionFlags values and structured event
// names. Outputs never attribute unmarked work to a nearby GPA command.
nlohmann::json indexRdcEvents(const nlohmann::json &roots, const nlohmann::json &nativeEvents);
nlohmann::json indexDx12RdcEvents(const nlohmann::json &roots, const nlohmann::json &nativeEvents);
nlohmann::json rdcProvenance(uint32_t event, const nlohmann::json &reverse);
nlohmann::json selectDx12RdcExecution(const nlohmann::json &mapping, const std::string &execution,
                                     const nlohmann::json &indirectAction = nullptr);
// -1 denotes a legacy/direct action; generated ordinals must have complete provenance.
int64_t indirectActionOrdinal(const nlohmann::json &provenance);
// Embedded capture policy, independent of sidecars and analysis job filenames.
std::string rdcQueryIsolationMarker(const nlohmann::json &policy);
nlohmann::json readRdcQueryIsolation(const nlohmann::json &roots);
void requireRdcQueryIsolation(const nlohmann::json &policy, const nlohmann::json &job);
} // namespace flora
