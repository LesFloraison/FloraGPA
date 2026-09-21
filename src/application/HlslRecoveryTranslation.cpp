#include "HlslCompilation.h"
#include "HlslRecoveryInternal.h"
#include "ReplayMesh.h"
#include "ShaderInspector.h"
#include "SystemDisassembly.h"
#include <QString>
#include <bit>

namespace flora::hlsl {
std::tuple<std::string, std::string, std::string> Lowerer::setupTessellation(unsigned forks,
                                                                             const Strings &body) {
    const auto decls = join(declarations, "\n");
    if (decls.find("dcl_tessellator_domain domain_tri") == std::string::npos)
        fail("Only triangular tessellation is currently recovered");
    auto m = match(R"(dcl_input_control_point_count (\d+))", decls, false);
    if (m.empty() || number(m[1]) != 3)
        fail("Expected three input control points");
    auto structure = [&](const std::string &key, const std::string &name) {
        auto entries = info.at("signatures").value(key, Json::array());
        Strings fields{"struct " + name + " {"};
        for (size_t n = 0; n < entries.size(); ++n) {
            auto &e = entries[n];
            static const std::map<unsigned, std::string> types{{1, "uint"}, {2, "int"}, {3, "float"}};
            const auto kind = types.at(e.at("component_type").get<unsigned>());
            std::string mask;
            for (unsigned bit = 0; bit < 4; ++bit)
                if (e.at("mask").get<unsigned>() & (1u << bit))
                    mask += "xyzw"[bit];
            const auto field = "field" + std::to_string(n);
            e["field"] = field;
            e["hlsl_type"] = kind;
            e["lanes"] = mask;
            fields.push_back("  " + kind + std::to_string(mask.size()) + ' ' + field + " : " +
                             e.at("semantic").get<std::string>() +
                             std::to_string(e.at("index").get<unsigned>()) + ';');
        }
        fields.push_back("};");
        globals.push_back(join(fields, "\n"));
        info["signatures"][key] = entries;
        return entries;
    };
    const auto input = structure("ISGN", "ControlInput"), output = structure("OSGN", "ControlOutput");
    globals.push_back(
        "struct PatchConstants { float edges[3]:SV_TessFactor; float inside:SV_InsideTessFactor; };");
    if (input.empty())
        fail("Missing tessellation input signature");
    uint32_t registers = 0;
    for (const auto &e : input) {
        const auto reg = e.at("register").get<uint32_t>();
        if (reg > 31)
            fail("Unsupported tessellation input register");
        registers = std::max(registers, reg + 1);
    }
    auto local = "\nuint4 vicp[3][" + std::to_string(registers) + "];\n";
    for (unsigned cp = 0; cp < 3; ++cp) {
        for (unsigned reg = 0; reg < registers; ++reg)
            local += "vicp[" + std::to_string(cp) + "][" + std::to_string(reg) + "]=0;\n";
        for (const auto &e : input) {
            auto value = "inputPatch[" + std::to_string(cp) + "]." + e.at("field").get<std::string>();
            if (e.at("hlsl_type") != "uint")
                value = "asuint(" + value + ')';
            local += "vicp[" + std::to_string(cp) + "][" + std::to_string(e.at("register").get<unsigned>()) +
                     "]." + e.at("lanes").get<std::string>() + '=' + value + ";\n";
        }
    }
    auto required = [&](const std::string &pattern) {
        const auto found = match(pattern, decls, false);
        if (found.empty())
            fail("Missing tessellation declaration: " + pattern);
        return found[1];
    };
    if (stage == "hs") {
        if (forks != 1 ||
            std::any_of(body.begin(), body.end(), [](const auto &s) { return s.starts_with("hs_"); }))
            fail("Only a single fork phase with implicit control-point passthrough is supported");
        const auto partition = required(R"(dcl_tessellator_partitioning partitioning_(\w+))");
        const auto topology = required(R"(dcl_tessellator_output_primitive output_(\w+))");
        if (number(required(R"(dcl_output_control_point_count (\d+))")) != 3)
            fail("Unsupported control-point count");
        const auto factors = info.at("signatures").value("PCSG", Json::array());
        if (factors.size() != 4)
            fail("Unsupported patch constant register layout");
        for (unsigned n = 0; n < 4; ++n) {
            const auto &e = factors[n];
            if (e.at("semantic") != (n < 3 ? "SV_TessFactor" : "SV_InsideTessFactor") ||
                e.at("index") != (n < 3 ? n : 0u) || e.at("register") != n || e.at("mask") != 1u)
                fail("Unsupported patch constant register layout");
        }
        const auto factor = required(R"(dcl_hs_max_tessfactor l\(([^)]+)\))");
        const double maxFactor = factor.starts_with("0x")
                                     ? double(std::bit_cast<float>(uint32_t(std::stoul(factor, nullptr, 16))))
                                     : std::stod(factor);
        Strings passthrough{"ControlOutput value;"};
        for (const auto &out : output) {
            const auto source = std::find_if(input.begin(), input.end(), [&](const auto &e) {
                return e.at("register") == out.at("register") && e.at("lanes") == out.at("lanes");
            });
            if (source == input.end())
                fail("Implicit HS passthrough signature mismatch");
            passthrough.push_back("value." + out.at("field").get<std::string>() + "=inputPatch[id]." +
                                  source->at("field").get<std::string>() + ';');
        }
        const auto suffix = "}\n[domain(\"tri\")][partitioning(\"" + partition + "\")][outputtopology(\"" +
                            topology +
                            "\")][outputcontrolpoints(3)][patchconstantfunc(\"PatchMain\")][maxtessfactor(" +
                            replayMeshFloat(maxFactor) +
                            ")]\nControlOutput main(InputPatch<ControlInput,3> inputPatch,uint "
                            "id:SV_OutputControlPointID) {\n" +
                            join(passthrough, "\n") + "\nreturn value;\n}";
        local += "\nuint4 o0=0,o1=0,o2=0,o3=0;";
        returnCode = "{ PatchConstants result; result.edges[0]=asfloat(o0.x); result.edges[1]=asfloat(o1.x); "
                     "result.edges[2]=asfloat(o2.x); result.inside=asfloat(o3.x); return result; }";
        return {"PatchConstants PatchMain(InputPatch<ControlInput,3> inputPatch) {", local, suffix};
    }
    if (output.empty())
        fail("Missing tessellation output signature");
    uint32_t outputRegisters = 0;
    for (const auto &e : output) {
        const auto reg = e.at("register").get<uint32_t>();
        if (reg > 31)
            fail("Unsupported tessellation output register");
        outputRegisters = std::max(outputRegisters, reg + 1);
    }
    local += "\nuint4 vDomain=asuint(float4(domainLocation,0));\n";
    Strings outputLocals;
    for (unsigned n = 0; n < outputRegisters; ++n)
        outputLocals.push_back("uint4 o" + std::to_string(n) + "=0;");
    local += join(outputLocals, "\n");
    Strings finish{"{ ControlOutput result;"};
    for (const auto &e : output) {
        auto value =
            'o' + std::to_string(e.at("register").get<unsigned>()) + '.' + e.at("lanes").get<std::string>();
        if (e.at("hlsl_type") != "uint")
            value = (e.at("hlsl_type") == "float" ? "asfloat(" : "asint(") + value + ')';
        finish.push_back("result." + e.at("field").get<std::string>() + '=' + value + ';');
    }
    finish.push_back("return result; }");
    returnCode = join(finish, " ");
    return {"[domain(\"tri\")]\nControlOutput main(PatchConstants patchConstants,float3 "
            "domainLocation:SV_DomainLocation,const OutputPatch<ControlInput,3> inputPatch) {",
            local, "}"};
}
std::string Lowerer::translate() {
    for (const auto &binding : info.at("bindings"))
        globals.push_back("// Captured binding " + binding.at("name").get<std::string>() + "; slot " +
                          std::to_string(binding.at("slot").get<unsigned>()) + "; type " +
                          std::to_string(binding.at("type").get<unsigned>()));
    for (const auto &block : info.at("constant_buffers")) {
        Strings variables;
        for (const auto &v : block.at("variables"))
            variables.push_back(v.at("name").get<std::string>() + " @ byte " +
                                std::to_string(v.at("offset").get<uint64_t>()));
        globals.push_back("// " + block.at("name").get<std::string>() + ": " + join(variables, ", "));
    }
    Strings body;
    unsigned forks = 0;
    for (const auto &line : lines) {
        if (line == "hs_fork_phase") {
            ++forks;
            continue;
        }
        if (line.starts_with("dcl_") || line == info.at("profile").get<std::string>() || line == "hs_decls") {
            if (!declaration(line))
                fail("Unsupported DXBC declaration: " + line);
        } else
            body.push_back(line);
    }
    body = prepareMemory(body);
    Strings tempDeclarations;
    for (unsigned n = 0; n < temps; ++n)
        tempDeclarations.push_back("uint4 r" + std::to_string(n) + "=0;");
    auto local = join(tempDeclarations, "\n");
    for (const auto &[name, shape] : indexableTemps)
        local += "\nuint4 " + name + '[' + std::to_string(shape.first) + "] = {" +
                 join(Strings(shape.first, "(uint4)0"), ",") + "};";
    std::string prefix, suffix, stageLocal;
    if (stage == "gs")
        std::tie(prefix, stageLocal, suffix) = setupGeometry();
    else if (stage == "vs" || stage == "ps")
        std::tie(prefix, stageLocal, suffix) = setupGraphics();
    else if (stage == "cs") {
        if (!threads)
            fail("Missing compute group dimensions");
        Strings parameters;
        const std::array<std::tuple<std::string, std::string, unsigned>, 4> system{
            {{"vThreadID", "SV_DispatchThreadID", 3},
             {"vThreadGroupID", "SV_GroupID", 3},
             {"vThreadIDInGroup", "SV_GroupThreadID", 3},
             {"vThreadIDInGroupFlattened", "SV_GroupIndex", 1}}};
        for (const auto &[name, semantic, width] : system) {
            if (inputs.contains(name)) {
                parameters.push_back("uint" + std::to_string(width) + " arg_" + name + " : " + semantic);
                stageLocal +=
                    "\nuint4 " + name + "=uint4(arg_" + name + (width == 3 ? ",0" : ",0,0,0") + ");";
            }
        }
        prefix = "[numthreads(" + std::to_string((*threads)[0]) + ',' + std::to_string((*threads)[1]) + ',' +
                 std::to_string((*threads)[2]) + ")]\nvoid main(" + join(parameters, ",") + ") {";
        returnCode = "return;";
        suffix = "}";
    } else
        std::tie(prefix, stageLocal, suffix) = setupTessellation(forks, body);
    local += stageLocal;
    Strings code;
    using Lanes = std::set<std::pair<std::string, char>>;
    Lanes sampleLanes, sampleProducts, floatLanes, arrayLanes;
    bool preserveBitMath = false;
    auto sampled = [&](const std::string &operand, const Lanes &provenance) {
        std::array<bool, 4> result{};
        const auto m = match(R"(-?(r\d+)\.([xyzw]{1,4}))", operand);
        if (m.empty())
            return result;
        auto mask = m[2];
        if (mask.size() == 1)
            mask.resize(4, mask[0]);
        // Valid source operands have one or four lanes; raw() rejects other widths.
        for (size_t n = 0; n < mask.size(); ++n)
            result[n] = provenance.contains({m[1], mask[n]});
        return result;
    };
    const std::array<bool, 4> yes{true, true, true, true}, no{};
    for (size_t n = 0; n < body.size(); ++n) {
        const auto ins = parseInstruction(body[n]);
        const auto &op = ins.opcode;
        const auto &a = ins.arguments;
        if (op == "ishr" || op == "ushr" || op == "ishl") {
            const auto bits = sampled(a.at(1), floatLanes);
            const auto dest = a.at(0).substr(a[0].rfind('.') + 1);
            for (auto lane : dest)
                if (bits.at(std::string("xyzw").find(lane)))
                    preserveBitMath = true;
        }
        orderedSampleMad = false;
        if (stage == "cs" && (op == "mad" || op == "mad_sat")) {
            const auto dest = a.at(0).substr(a[0].rfind('.') + 1);
            const auto left = sampled(a.at(1), sampleLanes), right = sampled(a.at(2), sampleLanes),
                       products = sampled(a.at(3), sampleProducts);
            const auto arrays = !match(R"(^-?x\d+\[)", a[3], false).empty() ? yes : sampled(a[3], arrayLanes);
            for (auto lane : dest) {
                const auto index = std::string("xyzw").find(lane);
                orderedSampleMad |=
                    (products.at(index) && (left.at(index) || right.at(index))) || arrays.at(index);
            }
        }
        code.push_back("// DXBC " + std::to_string(n) + ": " + body[n]);
        code.push_back(instruction(body[n]));
        if (!a.empty()) {
            const auto dest = match(R"((r\d+)\.([xyzw]{1,4}))", a[0]);
            if (!dest.empty()) {
                const auto copy = op == "mov"                   ? sampled(a.at(1), sampleLanes)
                                  : op.starts_with("sample_l_") ? yes
                                                                : no;
                const auto arrays = op == "mov" ? (!match(R"(^-?x\d+\[)", a.at(1), false).empty()
                                                       ? yes
                                                       : sampled(a.at(1), arrayLanes))
                                                : no;
                static const std::set<std::string> floating{
                    "add", "mul",      "div",      "mad",     "min",      "max",  "dp2",
                    "dp3", "dp4",      "sqrt",     "rsq",     "rcp",      "frc",  "exp",
                    "log", "round_ni", "round_pi", "round_z", "round_ne", "utof", "itof"};
                const auto bare = op.ends_with("_sat") ? op.substr(0, op.size() - 4) : op;
                const auto floats = op == "mov"               ? sampled(a.at(1), floatLanes)
                                    : floating.contains(bare) ? yes
                                                              : no;
                auto products = no;
                if (op == "mov")
                    products = sampled(a.at(1), sampleProducts);
                else if (op == "mul") {
                    const auto left = sampled(a.at(1), sampleLanes), right = sampled(a.at(2), sampleLanes);
                    for (unsigned k = 0; k < 4; ++k)
                        products[k] = left[k] || right[k];
                }
                for (auto lane : dest[2]) {
                    const auto key = std::make_pair(dest[1], lane);
                    const auto index = std::string("xyzw").find(lane);
                    sampleLanes.erase(key);
                    sampleProducts.erase(key);
                    floatLanes.erase(key);
                    arrayLanes.erase(key);
                    if (arrays[index])
                        arrayLanes.insert(key);
                    if (copy[index])
                        sampleLanes.insert(key);
                    if (products[index])
                        sampleProducts.insert(key);
                    if (floats[index])
                        floatLanes.insert(key);
                }
            }
            if (op == "imul" || op == "umul" || op == "udiv" || op == "sincos") {
                const auto second = match(R"((r\d+)\.([xyzw]{1,4}))", a.at(1));
                if (!second.empty())
                    for (auto lane : second[2]) {
                        const auto key = std::make_pair(second[1], lane);
                        sampleLanes.erase(key);
                        sampleProducts.erase(key);
                        floatLanes.erase(key);
                        arrayLanes.erase(key);
                    }
            }
        }
        if (op == "if_z" || op == "if_nz" || op == "else" || op == "endif" || op == "loop" ||
            op == "endloop" || op == "break" || op == "ret") {
            sampleLanes.clear();
            sampleProducts.clear();
            floatLanes.clear();
            arrayLanes.clear();
        }
    }
    std::string header = "// Reconstructed from DXBC. Register values are raw bits; not original source.\n";
    if (stage == "ps" || preserveBitMath || skipOptimization) {
        header += "// FloraGPA compiler optimization: preserve\n";
        header += stage == "ps" ? "// Preserve pixel arithmetic and sampling computations for analysis.\n"
                                : "// Floating results are consumed as integer bits; preserve their "
                                  "computation for analysis.\n";
    }
    return header + join(globals, "\n") + '\n' + prefix + '\n' + local + '\n' + join(code, "\n") + '\n' +
           suffix + '\n';
}
} // namespace flora::hlsl

namespace flora {
std::string recoverHlsl(Bytes bytecode) {
    auto info = inspectShader(bytecode);
    const auto signatures = info.at("signatures");
    info["signatures"] = {{"ISGN", signatures.value("input", nlohmann::json::array())},
                          {"OSGN", signatures.value("output", nlohmann::json::array())},
                          {"PCSG", signatures.value("patch", nlohmann::json::array())}};
    return hlsl::Lowerer(std::move(info), systemDisassembly(bytecode, 0x80)).translate();
}
RecoveredHlsl reconstructHlsl(Bytes bytecode, const nlohmann::json &savedSource) {
    const auto profile = inspectShader(bytecode).at("profile").get<std::string>();
    auto result = [](const std::string &source, HlslCompilation compiled, bool saved,
                     const std::string &entry) {
        RecoveredHlsl out;
        out.source = source;
        out.recompiled = std::move(compiled.bytecode);
        out.report = {
            {"source_kind", saved ? "saved_applied_hlsl" : "reconstructed_hlsl_not_original"},
            {"tool", saved ? "FloraGPA experiment source" : "FloraGPA native DXBC lowering"},
            {"native", true},
            {"recompiles", true},
            {"diagnostics", compiled.diagnostics},
            {"compilation", compiled.options},
            {"semantic_equivalence", saved ? "bytecode_identical" : "not_verified"},
            {"source_sha256", sha256({reinterpret_cast<const uint8_t *>(source.data()), source.size()})}};
        if (saved)
            out.report["entry"] = entry;
        return out;
    };
    if (savedSource.is_object() && savedSource.value("source_language", std::string{}) == "hlsl" &&
        savedSource.contains("source_text")) {
        try {
            const auto source = savedSource.at("source_text").get<std::string>();
            const auto entry = savedSource.value("source_entry", std::string("main"));
            auto compiled = compileHlsl(source, profile, entry);
            if (std::ranges::equal(bytecode, compiled.bytecode))
                return result(source, std::move(compiled), true, entry);
        } catch (const std::exception &) {
            // Stale or invalid saved text must not be presented as bound source.
        }
    }
    const auto source = recoverHlsl(bytecode);
    return result(source, compileHlsl(source, profile), false, "main");
}
} // namespace flora
