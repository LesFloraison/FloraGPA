#include "ShaderInspector.h"
#include "ShaderDebugData.h"
#include "StreamOutputInspector.h"
#include "core/ClassLinkage.h"
#include "replay/Replay.h"
#include <QByteArray>
#include <QStringDecoder>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <set>

namespace flora {
using Json = nlohmann::json;
using namespace shader_debug;
static Json reflectedType(ID3D11ShaderReflectionType *type, unsigned depth = 0) {
    if (depth > 64)
        throw std::runtime_error("Constant type nesting exceeds 64");
    D3D11_SHADER_TYPE_DESC t{};
    check(type->GetDesc(&t), "Reflect shader type");
    Json result{{"class_id", t.Class},  {"base_type", t.Type},    {"rows", t.Rows},
                {"columns", t.Columns}, {"elements", t.Elements}, {"members", t.Members}};
    if (t.Members) {
        result["member_types"] = Json::array();
        for (UINT i = 0; i < t.Members; ++i) {
            auto member = type->GetMemberTypeByIndex(i);
            D3D11_SHADER_TYPE_DESC desc{};
            check(member->GetDesc(&desc), "Reflect structure member");
            auto name = type->GetMemberTypeName(i);
            result["member_types"].push_back({{"name", name ? name : ""},
                                              {"offset", desc.Offset},
                                              {"type", reflectedType(member, depth + 1)}});
        }
    }
    return result;
}
Json inspectShader(Bytes bytes) {
    auto parts = chunks(bytes);
    Json result{
        {"sha256", sha256(bytes)},   {"chunks", Json::object()},          {"interface_slots", 0},
        {"bindings", Json::array()}, {"constant_buffers", Json::array()}, {"signatures", Json::object()}};
    for (auto &[name, data] : parts)
        result["chunks"][name] = data.size();
    result["embedded_sources"] = embedded(parts);
    auto it = parts.find("SHEX");
    if (it == parts.end())
        it = parts.find("SHDR");
    if (it == parts.end()) {
        result["profile"] = nullptr;
        result["stage"] = "signature";
        return result;
    }
    Reader code(it->second);
    auto version = code.read<UINT>();
    if (uint64_t(code.read<UINT>()) * 4 != it->second.size())
        throw std::runtime_error("Shader program length mismatch");
    const std::string names[] = {"ps", "vs", "gs", "hs", "ds", "cs"};
    auto type = version >> 16;
    if (type >= 6)
        throw std::runtime_error("Unknown shader program type");
    result["stage"] = names[type];
    result["profile"] =
        names[type] + "_" + std::to_string((version >> 4) & 15) + "_" + std::to_string(version & 15);
    Com<ID3D11ShaderReflection> reflection;
    check(D3DReflect(bytes.data(), bytes.size(), IID_ID3D11ShaderReflection, &reflection), "Reflect shader");
    D3D11_SHADER_DESC desc{};
    check(reflection->GetDesc(&desc), "Read shader descriptor");
    result["instructions"] = desc.InstructionCount;
    result["interface_slots"] = parts.contains("IFCE") ? reflection->GetNumInterfaceSlots() : 0;
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{};
        check(reflection->GetResourceBindingDesc(i, &b), "Reflect resource binding");
        result["bindings"].push_back({{"name", b.Name ? b.Name : ""},
                                      {"type", b.Type},
                                      {"slot", b.BindPoint},
                                      {"count", b.BindCount},
                                      {"dimension", b.Dimension},
                                      {"return_type", b.ReturnType}});
    }
    for (UINT i = 0; i < desc.ConstantBuffers; ++i) {
        auto buffer = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC b{};
        check(buffer->GetDesc(&b), "Reflect constant buffer");
        Json cb{{"name", b.Name ? b.Name : ""}, {"size", b.Size}, {"variables", Json::array()}};
        for (UINT j = 0; j < b.Variables; ++j) {
            auto variable = buffer->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC v{};
            check(variable->GetDesc(&v), "Reflect shader variable");
            D3D11_SHADER_TYPE_DESC t{};
            check(variable->GetType()->GetDesc(&t), "Reflect shader type");
            cb["variables"].push_back({{"name", v.Name ? v.Name : ""},
                                       {"offset", v.StartOffset},
                                       {"size", v.Size},
                                       {"flags", v.uFlags},
                                       {"type", t.Type},
                                       {"class", t.Class},
                                       {"rows", t.Rows},
                                       {"columns", t.Columns},
                                       {"elements", t.Elements},
                                       {"type_layout", reflectedType(variable->GetType())}});
        }
        result["constant_buffers"].push_back(cb);
    }
    for (int kind = 0; kind < 3; ++kind) {
        Json sig = Json::array();
        auto count = kind == 0   ? desc.InputParameters
                     : kind == 1 ? desc.OutputParameters
                                 : desc.PatchConstantParameters;
        for (UINT i = 0; i < count; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            check(kind == 0   ? reflection->GetInputParameterDesc(i, &p)
                  : kind == 1 ? reflection->GetOutputParameterDesc(i, &p)
                              : reflection->GetPatchConstantParameterDesc(i, &p),
                  "Reflect signature");
            sig.push_back({{"semantic", p.SemanticName ? p.SemanticName : ""},
                           {"index", p.SemanticIndex},
                           {"register", p.Register},
                           {"system_value", p.SystemValueType},
                           {"component_type", p.ComponentType},
                           {"mask", p.Mask},
                           {"stream", p.Stream}});
        }
        result["signatures"][kind == 0 ? "input" : kind == 1 ? "output" : "patch"] = sig;
    }
    return result;
}
Json inspectResourceShader(const Frame &frame, Id id, Bytes bytes) {
    static const std::map<uint16_t, std::string> stages{{0x90, "vs"}, {0x91, "gs"}, {0x92, "ps"},
                                                        {0x93, "cs"}, {0x94, "ds"}, {0x95, "hs"}};
    const auto &entry = frame.entry(id);
    if (entry.category != 5 || !stages.contains(entry.type))
        throw std::runtime_error("Resource is not a shader");
    auto out = inspectShader(bytes);
    auto stage = out.at("stage").get<std::string>();
    auto so = entry.type == 0x91 ? shaderStreamOutput(frame, id) : 0;
    bool passthrough = so && (stage == "vs" || stage == "ds" || stage == "signature");
    if (stage != stages.at(entry.type) && !passthrough)
        throw std::runtime_error("Replacement shader stage mismatch");
    out["pipeline_stage"] = stages.at(entry.type);
    out["bytecode_stage"] = stage;
    out["passthrough"] = passthrough;
    out["class_linkage_id"] = shaderClassLinkage(frame, id);
    if (so)
        out["stream_output"] = streamOutputJson(readStreamOutputDeclaration(frame, so));
    if (passthrough) {
        out["interface_slots"] = 0;
        out["execution_note"] = "Only the preceding stage output signature is used; these bytecode "
                                "instructions do not execute in GS.";
    }
    return out;
}
} // namespace flora
