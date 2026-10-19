#include "DebugTrace.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <set>

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
static Json compareOutput(const ShaderReflection &reflection, const Json &trace,
                          const Json &steps, const Json &reference, bool position, int explicitSignature = -1) {
    Json result{{"status", "unavailable"}, {"output_index", 0},
                {"comparison", "exact_32_bit_components"},
                {"scope", "Final mapped shader output versus a separate instrumented GPU replay; "
                          "equality does not validate intermediate instructions or input stability."}};
    if (position) { result.erase("output_index"); result["semantic"] = "SV_Position"; }
    auto unavailable = [&](const char *reason) {
        result["reason"] = reason;
        return result;
    };
    if (reference.value("status", "") != "read")
        return unavailable("GPU output reference is not eligible for comparison");
    if (position && reflection.stage != ShaderStage::Vertex)
        return unavailable("Position comparison requires a vertex shader");
    int signatureIndex = explicitSignature;
    for (size_t i = 0; explicitSignature < 0 && i < reflection.outputSignature.size(); ++i) {
        const auto &signature = reflection.outputSignature[i];
        if (position ? signature.systemValue == ShaderBuiltin::Position && signature.stream == 0
                     : signature.systemValue == ShaderBuiltin::ColorOutput && signature.regIndex == 0) {
            if (signatureIndex >= 0)
                return unavailable("Selected output signature is ambiguous");
            signatureIndex = int(i);
        }
    }
    if (signatureIndex < 0)
        return unavailable(position ? "No reflected SV_Position output" : "No reflected target-0 color output");
    if (size_t(signatureIndex) >= reflection.outputSignature.size())
        return unavailable("Output signature index is out of range");
    const auto &signature = reflection.outputSignature[size_t(signatureIndex)];
    if (explicitSignature >= 0) {
        result["semantic"] = std::string(signature.semanticName.c_str());
        result["semantic_index"] = signature.semanticIndex;
        result["signature_index"] = signatureIndex;
    }
    const auto count = signature.compCount;
    const auto type = signature.varType;
    if (position && explicitSignature < 0 && (type != VarType::Float || count != 4))
        return unavailable("Only float4 SV_Position outputs are compared");
    if (!count || count > 4 || signature.regChannelMask != (1U << count) - 1 ||
        (type != VarType::Float && type != VarType::UInt && type != VarType::SInt))
        return unavailable("Only contiguous 32-bit output signature components are compared");
    const auto interpretation = type == VarType::Float ? "float" : type == VarType::UInt ? "uint" : "sint";
    if (reference.value("interpretation", "") != interpretation)
        return unavailable("GPU reference and output signature interpretations differ");
    const Json *mapping = nullptr;
    for (const auto &candidate : trace.at("sourceVars")) {
        if (candidate.at("signatureIndex") != signatureIndex || candidate.at("undefinedValue") != false ||
            candidate.at("type") != uint32_t(type) || candidate.at("rows") != 1 || candidate.at("columns") != count)
            continue;
        const auto &variables = candidate.at("variables");
        if (variables.size() != count ||
            !std::all_of(variables.begin(), variables.end(), [](const auto &v) {
                return v.at("type") == uint32_t(DebugVariableType::Variable);
            }))
            continue;
        if (mapping)
            return unavailable("Selected debugger output mapping is ambiguous");
        mapping = &candidate;
    }
    if (!mapping)
        return unavailable("No complete debugger mapping for selected output");
    Json actual = Json::array(), expected = Json::array(), mismatched = Json::array();
    for (size_t lane = 0; lane < count; ++lane) {
        const auto &binding = mapping->at("variables")[lane];
        const auto name = binding.at("name").get<std::string>();
        const auto component = binding.at("component").get<uint32_t>();
        const auto split = name.find('.');
        const auto root = name.substr(0, split);
        const Json *value = nullptr;
        bool observed = false;
        // Last change wins, including removal. Do not recover stale values from
        // earlier steps when the final output is absent or has a new shape.
        for (auto step = steps.rbegin(); step != steps.rend() && !observed; ++step)
            for (auto change = step->at("changes").rbegin(); change != step->at("changes").rend(); ++change) {
                const auto &after = change->at("after");
                if (after.at("name") != root && change->at("before").at("name") != root)
                    continue;
                observed = true;
                if (after.at("name") == root) value = &after;
                break;
            }
        size_t cursor = split;
        while (value && cursor != std::string::npos) {
            const auto next = name.find('.', cursor + 1);
            const auto memberName = name.substr(cursor + 1, next == std::string::npos ? next : next - cursor - 1);
            const Json *memberValue = nullptr;
            for (const auto &member : value->at("members"))
                if (member.at("name") == memberName) {
                    if (memberValue) return unavailable("Ambiguous debugger output member");
                    memberValue = &member;
                }
            value = memberValue;
            cursor = next;
        }
        if (!value || value->at("type") != uint32_t(type) || value->at("rows") != 1 ||
            component >= value->at("columns").get<uint32_t>() || component >= 4)
            return unavailable("Final debugger output component is unavailable");
        actual.push_back(value->at("value").at("u32v").at(component));
        expected.push_back(reference.at("value").at("uintValue").at(lane));
        if (actual.back() != expected.back()) mismatched.push_back(lane);
    }
    result["status"] = mismatched.empty() ? "exact_output_match" : "output_mismatch";
    result["interpretation"] = interpretation;
    result["trace_bits"] = actual;
    result["gpu_bits"] = expected;
    result["mismatched_components"] = mismatched;
    return result;
}
Json rdcComparePixelOutput(const ShaderReflection &reflection, const Json &trace,
                          const Json &steps, const Json &reference) {
    return compareOutput(reflection, trace, steps, reference, false);
}
Json rdcCompareVertexPosition(const ShaderReflection &reflection, const Json &trace,
                             const Json &steps, const Json &reference) {
    return compareOutput(reflection, trace, steps, reference, true);
}
Json rdcCompareVertexOutputs(const ShaderReflection &reflection, const Json &trace,
                             const Json &steps, const Json &reference) {
    Json result{{"status", "unavailable"}, {"outputs", Json::array()}, {"matched", 0}, {"mismatched", 0}, {"unavailable", 0}};
    if (reference.value("status", "") != "read" || !reference.contains("outputs") ||
        reference.at("outputs").size() != reflection.outputSignature.size() || reference.at("outputs").empty()) {
        result["reason"] = "No complete GPU output layout is available"; return result;
    }
    std::set<int> seen;
    for (const auto &output : reference.at("outputs")) {
        const auto index = output.at("signature_index").get<int>();
        if (index < 0 || size_t(index) >= reflection.outputSignature.size() || !seen.insert(index).second) {
            result["reason"] = "GPU output layout has invalid or duplicate signatures"; return result;
        }
    }
    unsigned matched{}, mismatched{}, unavailable{};
    for (const auto &output : reference.at("outputs")) {
        auto comparison = compareOutput(reflection, trace, steps, output, true, output.at("signature_index").get<int>());
        comparison["semantic"] = output.at("semantic"); comparison["semantic_index"] = output.at("semantic_index");
        const auto status = comparison.at("status");
        if (status == "exact_output_match") ++matched;
        else if (status == "output_mismatch") ++mismatched;
        else ++unavailable;
        result["outputs"].push_back(std::move(comparison));
    }
    result.update({{"status", mismatched ? "output_mismatch" : unavailable ? "partial_comparison" : "exact_output_match"},
                   {"matched", matched}, {"mismatched", mismatched}, {"unavailable", unavailable}});
    return result;
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
