#pragma once
#include <nlohmann/json.hpp>
#include <string_view>
namespace flora {
inline bool rdcShaderDebugAction(std::string_view action) {
    return action == "debug-pixel" || action == "debug-vertex" || action == "debug-thread";
}
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
std::string rdcFrameEndMarker(const nlohmann::json &boundary);
nlohmann::json readRdcFrameEnd(const nlohmann::json &roots);
void requireRdcFrameEnd(const nlohmann::json &marker, const nlohmann::json &job);
std::string rdcResourceBoundaryMarker(const nlohmann::json &boundary);
nlohmann::json readRdcResourceBoundary(const nlohmann::json &roots, const nlohmann::json &job);
// Verifies every emitted partial resolve against the isolated capture policy.
// operations are ordered captured destination ranges, each with its eventId.
void verifyRdcQueryResolveExpansion(const nlohmann::json &command, const nlohmann::json &entry,
                                    const nlohmann::json &operations, const std::string &resourceId);
} // namespace flora
