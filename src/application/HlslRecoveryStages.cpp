#include "HlslRecoveryInternal.h"

namespace flora::hlsl {
namespace {
std::string laneMask(const Json &entry) {
    std::string result;
    for (unsigned n = 0; n < 4; ++n)
        if (entry.at("mask").get<unsigned>() & (1u << n))
            result += "xyzw"[n];
    return result;
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
std::string componentKind(const Json &entry) {
    static const std::map<unsigned, std::string> types{{1, "uint"}, {2, "int"}, {3, "float"}};
    const auto type = entry.at("component_type").get<unsigned>();
    if (!types.contains(type))
        fail("Unsupported signature component type");
    return types.at(type);
}
std::pair<Strings, Json> geometryFields(const Json &entries) {
    Strings fields;
    Json mapped = Json::array();
    for (size_t n = 0; n < entries.size(); ++n) {
        const auto &e = entries[n];
        const auto kind = componentKind(e), mask = laneMask(e);
        if (mask.empty() || e.at("register").get<uint32_t>() > 31)
            fail("Unsupported GS signature register/type");
        const auto field = "field" + std::to_string(n);
        fields.push_back(kind + std::to_string(mask.size()) + ' ' + field + " : " +
                         e.at("semantic").get<std::string>() + std::to_string(e.at("index").get<uint32_t>()) +
                         ';');
        mapped.push_back({{"register", e.at("register")}, {"mask", mask}, {"kind", kind}, {"field", field}});
    }
    return {fields, mapped};
}
} // namespace
bool Lowerer::graphicsDeclaration(const std::string &line) {
    if (stage != "vs" && stage != "ps")
        return false;
    if (stage == "ps" &&
        (line == "dcl_output oDepth" || line == "dcl_output oDepthGE" || line == "dcl_output oDepthLE"))
        return true;
    if (!match(R"(dcl_output(?:_siv)? o\d+\.[xyzw]+(?:, \w+)?)", line).empty())
        return true;
    if (stage == "vs")
        return !match(R"(dcl_input(?:_sgv|_siv)? v\d+\.[xyzw]+(?:, (?:vertex_id|instance_id|position))?)",
                      line)
                    .empty();
    const auto m = match(
        R"(dcl_input_ps(?:_sgv|_siv)? (constant|linear(?: noperspective)?(?: centroid| sample)?) v(\d+)\.([xyzw]+)(?:, (?:position|is_front_face|primitive_id|sample_index))?)",
        line);
    if (m.empty())
        return false;
    for (auto lane : m[3])
        interpolation[{number(m[2]), lane}] = m[1];
    return true;
}
std::tuple<std::string, std::string, std::string> Lowerer::setupGraphics() {
    std::string local;
    Json input = Json::array(), output = Json::array();
    for (const auto key : {"ISGN", "OSGN"}) {
        const bool in = std::string(key) == "ISGN";
        auto &dest = in ? input : output;
        const auto name = in ? "GraphicsInput" : "GraphicsOutput";
        Strings fields;
        const auto entries = info.at("signatures").value(key, Json::array());
        for (size_t n = 0; n < entries.size(); ++n) {
            const auto &e = entries[n];
            const auto reg = e.at("register").get<uint32_t>();
            auto mask = laneMask(e);
            const auto semantic = e.at("semantic").get<std::string>();
            const auto lowered = lower(semantic);
            std::string depth;
            if (!in && stage == "ps") {
                static const std::map<std::string, std::string> depths{{"sv_depth", "oDepth"},
                                                                       {"sv_depthgreaterequal", "oDepthGE"},
                                                                       {"sv_depthlessequal", "oDepthLE"}};
                if (depths.contains(lowered))
                    depth = depths.at(lowered);
            }
            if (!depth.empty())
                mask = "x";
            if ((reg > 31 && depth.empty()) || mask.empty())
                fail("Unsupported graphics system register/signature");
            auto kind = componentKind(e);
            const bool front = lowered == "sv_isfrontface";
            if (front)
                kind = "bool";
            std::string qualifier;
            if (in && stage == "ps") {
                std::set<std::string> modes;
                for (auto lane : mask)
                    if (interpolation.contains({reg, lane}))
                        modes.insert(interpolation.at({reg, lane}));
                if (modes.size() > 1)
                    fail("Mixed interpolation in one signature field");
                const auto mode =
                    modes.empty()
                        ? (kind == "uint" || kind == "int" || kind == "bool" ? "constant" : "linear")
                        : *modes.begin();
                qualifier = (mode == "constant" ? "nointerpolation" : strip(replace(mode, "linear", ""))) +
                            std::string(" ");
            }
            const auto field = "field" + std::to_string(n);
            fields.push_back(qualifier + kind + (front ? "" : std::to_string(mask.size())) + ' ' + field +
                             " : " + semantic + std::to_string(e.at("index").get<uint32_t>()) + ';');
            dest.push_back(
                {{"register", reg}, {"mask", mask}, {"kind", kind}, {"field", field}, {"depth", depth}});
        }
        if (!fields.empty())
            globals.push_back(std::string("struct ") + name + " {\n" + join(fields, "\n") + "\n};");
    }
    for (bool in : {true, false}) {
        std::set<std::string> registers;
        for (const auto &e : in ? input : output) {
            auto name = e.at("depth").get<std::string>();
            if (name.empty())
                name = (in ? "v" : "o") + std::to_string(e.at("register").get<uint32_t>());
            registers.insert(name);
        }
        for (const auto &reg : registers)
            local += "uint4 " + reg + "=0;\n";
    }
    for (const auto &e : input) {
        std::string value = "input." + e.at("field").get<std::string>();
        if (e.at("kind") == "bool")
            value = '(' + value + " ? 0xffffffffu : 0u)";
        else if (e.at("kind") != "uint")
            value = "asuint(" + value + ')';
        local += 'v' + std::to_string(e.at("register").get<uint32_t>()) + '.' +
                 e.at("mask").get<std::string>() + '=' + value + ";\n";
    }
    Strings finish;
    if (!output.empty())
        finish.push_back("{ GraphicsOutput result;");
    for (const auto &e : output) {
        auto value = e.at("depth").get<std::string>();
        if (value.empty())
            value = 'o' + std::to_string(e.at("register").get<uint32_t>());
        value += '.' + e.at("mask").get<std::string>();
        if (e.at("kind") != "uint")
            value = (e.at("kind") == "float" ? "asfloat(" : "asint(") + value + ')';
        finish.push_back("result." + e.at("field").get<std::string>() + '=' + value + ';');
    }
    finish.push_back("return result; }");
    returnCode = output.empty() ? "return;" : join(finish, " ");
    return {std::string(output.empty() ? "void" : "GraphicsOutput") + " main(" +
                (input.empty() ? "" : "GraphicsInput input") + ") {",
            local, "}"};
}
bool Lowerer::geometryDeclaration(const std::string &line) {
    if (stage != "gs")
        return false;
    auto m = match("dcl_inputprimitive (point|line|triangle|lineadj|triangleadj)", line);
    if (!m.empty()) {
        primitive = m[1];
        return true;
    }
    m = match(R"(dcl_(maxout|gsinstances) (\d+))", line);
    if (!m.empty()) {
        const auto value = number(m[2]);
        const auto limit = m[1] == "gsinstances" ? 32 : 1024;
        if (!value || value > unsigned(limit))
            fail("Invalid geometry shader output/instance count");
        if (m[1] == "maxout")
            maxOutput = value;
        else
            instances = value;
        return true;
    }
    m = match("dcl_stream m([0-3])", line);
    if (!m.empty()) {
        currentStream = number(m[1]);
        streams.try_emplace(currentStream, "");
        return true;
    }
    m = match("dcl_outputtopology (pointlist|linestrip|trianglestrip)", line);
    if (!m.empty()) {
        streams[currentStream] = m[1];
        return true;
    }
    m = match(
        R"(dcl_input(?:_siv)? v\[([1-6])\]\[(\d+)\]\.[xyzw]+(?:, (?:position|clip_distance|cull_distance))?)",
        line);
    if (!m.empty()) {
        const auto count = number(m[1]);
        if (inputCount && *inputCount != count)
            fail("Inconsistent GS input primitive count");
        inputCount = count;
        return true;
    }
    if (line == "dcl_input vPrim" || line == "dcl_input vGSInstanceID") {
        systemInputs.insert(line.substr(10));
        return true;
    }
    return !match(
                R"(dcl_output(?:_siv)? o\d+\.[xyzw]+(?:, (?:position|clip_distance|cull_distance|render_target_array_index|viewport_array_index|primitive_id))?)",
                line)
                .empty();
}
std::tuple<std::string, std::string, std::string> Lowerer::setupGeometry() {
    static const std::map<std::string, unsigned> primitives{
        {"point", 1}, {"line", 2}, {"triangle", 3}, {"lineadj", 4}, {"triangleadj", 6}};
    static const std::map<std::string, std::string> topologies{
        {"pointlist", "PointStream"}, {"linestrip", "LineStream"}, {"trianglestrip", "TriangleStream"}};
    if (primitive.empty() || !maxOutput)
        fail("Missing geometry shader primitive/output declaration");
    const auto count = primitives.at(primitive);
    if (inputCount && *inputCount != count)
        fail("Geometry input declaration/primitive mismatch");
    if (streams.empty() ||
        std::any_of(streams.begin(), streams.end(), [](const auto &p) { return p.second.empty(); }))
        fail("Missing GS output topology");
    unsigned streamIndex = 0;
    for (const auto &[stream, topology] : streams) {
        if (stream != streamIndex++)
            fail("Sparse GS stream indices are not yet recovered");
        if (streams.size() > 1 && topology != "pointlist")
            fail("Multiple GS streams must emit points");
    }
    Json entries = Json::array();
    for (const auto &e : info.at("signatures").value("ISGN", Json::array())) {
        if (e.at("register") == UINT32_MAX &&
            lower(e.at("semantic").get<std::string>()) == "sv_primitiveid") {
            if (!systemInputs.contains("vPrim"))
                fail("GS primitive ID signature/declaration mismatch");
        } else
            entries.push_back(e);
    }
    const auto [fields, mapped] = geometryFields(entries);
    globals.push_back("struct GeometryInput {\n" + join(fields, "\n") + "\n};");
    unsigned registers = 0;
    for (const auto &e : mapped)
        registers = std::max(registers, e.at("register").get<unsigned>() + 1);
    std::string local;
    if (registers)
        local = "uint4 v[" + std::to_string(count) + "][" + std::to_string(registers) + "]={" +
                join(Strings(count * registers, "(uint4)0"), ",") + "};\n";
    for (unsigned vertex = 0; vertex < count; ++vertex) {
        for (unsigned reg = 0; reg < registers; ++reg)
            local += "v[" + std::to_string(vertex) + "][" + std::to_string(reg) + "]=0;\n";
        for (const auto &e : mapped) {
            auto value = "input[" + std::to_string(vertex) + "]." + e.at("field").get<std::string>();
            if (e.at("kind") != "uint")
                value = "asuint(" + value + ')';
            local += "v[" + std::to_string(vertex) + "][" + std::to_string(e.at("register").get<unsigned>()) +
                     "]." + e.at("mask").get<std::string>() + '=' + value + ";\n";
        }
    }
    Strings parameters{primitive + " GeometryInput input[" + std::to_string(count) + ']'};
    for (const auto &[name, semantic] : std::array<std::pair<std::string, std::string>, 2>{
             {{"vPrim", "SV_PrimitiveID"}, {"vGSInstanceID", "SV_GSInstanceID"}}}) {
        if (systemInputs.contains(name)) {
            parameters.push_back("uint arg_" + name + ':' + semantic);
            local += "uint4 " + name + "=(uint4)arg_" + name + ";\n";
        }
    }
    const auto outputEntries = info.at("signatures").value("OSGN", Json::array());
    for (const auto &e : outputEntries)
        if (!streams.contains(e.value("stream", 0u)))
            fail("Signature refers to undeclared GS stream");
    std::set<unsigned> outputRegisters;
    for (const auto &[stream, topology] : streams) {
        Json streamEntries = Json::array();
        for (const auto &e : outputEntries)
            if (e.value("stream", 0u) == stream)
                streamEntries.push_back(e);
        const auto [outputFields, output] = geometryFields(streamEntries);
        geometryOutputs[stream] = output;
        if (outputFields.empty())
            fail("GS output stream has no signature");
        const auto type = "GeometryOutput" + std::to_string(stream);
        globals.push_back("struct " + type + " {\n" + join(outputFields, "\n") + "\n};");
        parameters.push_back("inout " + topologies.at(topology) + '<' + type + "> stream" +
                             std::to_string(stream));
        for (const auto &e : output)
            outputRegisters.insert(e.at("register").get<unsigned>());
    }
    for (auto reg : outputRegisters)
        local += "uint4 o" + std::to_string(reg) + "=0;\n";
    returnCode = "return;";
    auto attributes = "[maxvertexcount(" + std::to_string(*maxOutput) + ")]";
    if (instances != 1 || systemInputs.contains("vGSInstanceID"))
        attributes += "[instance(" + std::to_string(instances) + ")]";
    return {attributes + "\nvoid main(" + join(parameters, ",") + ") {", local, "}"};
}
std::optional<std::string> Lowerer::geometryInstruction(const std::string &op, const Strings &args) const {
    if (!std::set<std::string>{"emit", "cut", "emitThenCut", "emit_stream", "cut_stream",
                               "emitThenCut_stream"}
             .contains(op))
        return std::nullopt;
    if (stage != "gs")
        fail("Geometry emission outside a GS");
    unsigned stream = 0;
    if (op.ends_with("_stream")) {
        if (args.size() != 1 || match("m[0-3]", args[0]).empty())
            fail("Invalid immediate GS stream");
        stream = unsigned(args[0][1] - '0');
    } else if (!args.empty())
        fail("Unexpected GS emit/cut operands");
    if (!geometryOutputs.contains(stream))
        fail("GS instruction uses an undeclared stream");
    Strings code;
    if (op.starts_with("emit")) {
        code.push_back("{ GeometryOutput" + std::to_string(stream) + " result;");
        for (const auto &e : geometryOutputs.at(stream)) {
            auto value = 'o' + std::to_string(e.at("register").get<unsigned>()) + '.' +
                         e.at("mask").get<std::string>();
            if (e.at("kind") != "uint")
                value = (e.at("kind") == "float" ? "asfloat(" : "asint(") + value + ')';
            code.push_back("result." + e.at("field").get<std::string>() + '=' + value + ';');
        }
        code.push_back("stream" + std::to_string(stream) + ".Append(result); }");
    }
    if (op.starts_with("cut") || op.starts_with("emitThenCut"))
        code.push_back("stream" + std::to_string(stream) + ".RestartStrip();");
    return join(code, "\n");
}
} // namespace flora::hlsl
