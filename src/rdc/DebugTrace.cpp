#include "DebugTrace.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace flora {
namespace {
using Json = nlohmann::json;
std::string text(const rdcstr &s) { return {s.c_str(), s.size()}; }
Json real(double value) {
    if (std::isfinite(value))
        return value;
    return std::isnan(value) ? "nan" : value < 0 ? "-inf" : "inf";
}
float half(uint16_t bits) {
    const auto sign = uint32_t(bits & 0x8000) << 16;
    const auto exponent = (bits >> 10) & 31;
    const auto fraction = bits & 1023;
    if (!exponent)
        return std::copysign(std::ldexp(float(fraction), -24), bits & 0x8000 ? -1.f : 1.f);
    const auto value = exponent == 31 ? 0x7f800000U | (uint32_t(fraction) << 13)
                                      : (uint32_t(exponent + 112) << 23) | (uint32_t(fraction) << 13);
    return std::bit_cast<float>(sign | value);
}
template <typename T> Json lanes(const ShaderValue &value) {
    Json result = Json::array();
    for (size_t i = 0; i < 16; ++i) {
        T lane;
        memcpy(&lane, reinterpret_cast<const unsigned char *>(&value) + i * sizeof(T), sizeof(T));
        if constexpr (std::is_floating_point_v<T>)
            result.push_back(real(lane));
        else
            result.push_back(lane);
    }
    return result;
}
Json values(const ShaderValue &value) {
    Json halves = Json::array();
    for (size_t i = 0; i < 16; ++i) {
        uint16_t lane;
        memcpy(&lane, reinterpret_cast<const unsigned char *>(&value) + i * sizeof(lane), sizeof(lane));
        halves.push_back(real(half(lane)));
    }
    return {{"f16v", halves},
            {"f32v", lanes<float>(value)},
            {"f64v", lanes<double>(value)},
            {"s8v", lanes<int8_t>(value)},
            {"u8v", lanes<uint8_t>(value)},
            {"s16v", lanes<int16_t>(value)},
            {"u16v", lanes<uint16_t>(value)},
            {"s32v", lanes<int32_t>(value)},
            {"u32v", lanes<uint32_t>(value)},
            {"s64v", lanes<int64_t>(value)},
            {"u64v", lanes<uint64_t>(value)}};
}
Json variable(const ShaderVariable &value, unsigned depth, size_t &count) {
    if (depth > 128 || ++count > 1000000)
        throw std::runtime_error("Shader variable tree exceeds bounds");
    Json members = Json::array();
    for (const auto &v : value.members)
        members.push_back(variable(v, depth + 1, count));
    return {{"name", text(value.name)},       {"rows", value.rows},
            {"columns", value.columns},       {"type", uint32_t(value.type)},
            {"flags", uint32_t(value.flags)}, {"value", values(value.value)},
            {"members", std::move(members)}};
}
template <typename T, typename F> Json array(const T &input, F convert) {
    Json result = Json::array();
    for (const auto &v : input)
        result.push_back(convert(v));
    return result;
}
Json sourceMapping(const SourceVariableMapping &v, bool global) {
    // DXBC's CreateShaderDebugState/FillCBufferVariables in the 1.45 backend
    // leave offset uninitialized for signature, coverage and whole-CB mappings.
    // Do not read those bytes or expose them as a meaningful struct offset.
    bool missingOffset = false;
    if (global) {
        missingOffset = v.signatureIndex >= 0;
        for (const auto &r : v.variables)
            missingOffset |= r.type == DebugVariableType::Input || r.type == DebugVariableType::Variable ||
                             (r.type == DebugVariableType::Constant && v.type == VarType::Unknown &&
                              !v.rows && !v.columns);
    }
    return {{"name", text(v.name)},
            {"type", uint32_t(v.type)},
            {"rows", v.rows},
            {"columns", v.columns},
            {"offset", missingOffset ? Json(nullptr) : Json(v.offset)},
            {"signatureIndex", v.signatureIndex},
            {"undefinedValue", v.undefinedValue},
            {"variables", array(v.variables, [](const auto &r) {
                 return Json{{"name", text(r.name)}, {"type", uint32_t(r.type)}, {"component", r.component}};
             })}};
}
Json source(const SourceVariableMapping &v) { return sourceMapping(v, false); }
Json line(const LineColumnInfo &v) {
    return {{"disassemblyLine", v.disassemblyLine},
            {"fileIndex", v.fileIndex},
            {"lineStart", v.lineStart},
            {"lineEnd", v.lineEnd},
            {"colStart", v.colStart},
            {"colEnd", v.colEnd}};
}
} // namespace
Json rdcVariable(const ShaderVariable &v) {
    size_t count = 0;
    return variable(v, 0, count);
}
Json rdcDebugTrace(const ShaderDebugTrace &trace) {
    return {{"stage", uint32_t(trace.stage)},
            {"inputs", array(trace.inputs, rdcVariable)},
            {"constantBlocks", array(trace.constantBlocks, rdcVariable)},
            {"readOnlyResources", array(trace.readOnlyResources, rdcVariable)},
            {"readWriteResources", array(trace.readWriteResources, rdcVariable)},
            {"samplers", array(trace.samplers, rdcVariable)},
            {"sourceVars", array(trace.sourceVars, [](const auto &v) { return sourceMapping(v, true); })},
            {"instInfo", array(trace.instInfo, [](const auto &v) {
                 return Json{{"instruction", v.instruction},
                             {"lineInfo", line(v.lineInfo)},
                             {"sourceVars", array(v.sourceVars, source)}};
             })}};
}
Json rdcDebugState(const ShaderDebugState &state) {
    return {{"nextInstruction", state.nextInstruction},
            {"stepIndex", state.stepIndex},
            {"flags", uint32_t(state.flags)},
            {"callstack", array(state.callstack, text)},
            {"changes", array(state.changes, [](const auto &v) {
                 return Json{{"before", rdcVariable(v.before)}, {"after", rdcVariable(v.after)}};
             })}};
}
Json rdcDebugInfo(const ShaderDebugInfo &info) {
    return {{"compileFlags",
             {{"flags",
               array(info.compileFlags.flags,
                     [](const auto &v) { return Json{{"name", text(v.name)}, {"value", text(v.value)}}; })}}},
            {"files", array(info.files,
                            [](const auto &v) {
                                return Json{{"filename", text(v.filename)}, {"contents", text(v.contents)}};
                            })},
            {"entrySourceName", text(info.entrySourceName)},
            {"entryLocation", line(info.entryLocation)},
            {"editBaseFile", info.editBaseFile},
            {"encoding", uint32_t(info.encoding)},
            {"compiler", uint32_t(info.compiler)},
            {"debuggable", info.debuggable},
            {"sourceDebugInformation", info.sourceDebugInformation},
            {"debugStatus", text(info.debugStatus)},
            {"debugInfoLoadingLog", text(info.debugInfoLoadingLog)}};
}
} // namespace flora
