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
} // namespace flora
