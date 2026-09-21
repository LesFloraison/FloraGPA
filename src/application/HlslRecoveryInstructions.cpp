#include "HlslRecoveryInternal.h"
#include <QRegularExpression>

namespace flora::hlsl {
bool Lowerer::declaration(const std::string &line) {
    if (line == info.at("profile").get<std::string>() || line == "hs_decls")
        return true;
    declarations.push_back(line);
    if (memoryDeclaration(line) || graphicsDeclaration(line) || geometryDeclaration(line))
        return true;
    if (line.starts_with("dcl_immediateConstantBuffer")) {
        const auto value = strip(line.substr(std::string("dcl_immediateConstantBuffer").size()));
        const QRegularExpression row(
            QStringLiteral(R"(\{\s*(0x[0-9a-fA-F]{8}(?:\s*,\s*0x[0-9a-fA-F]{8}){3})\s*\})"));
        auto found = row.globalMatch(QString::fromStdString(value));
        Strings rows;
        while (found.hasNext()) {
            const auto m = found.next();
            auto values = splitArguments(m.captured(1).toStdString());
            for (auto &s : values)
                s += 'u';
            rows.push_back("uint4(" + join(values, ",") + ')');
        }
        auto residue = QString::fromStdString(value);
        residue.remove(QRegularExpression("0x[0-9a-fA-F]{8}"));
        residue.remove(QRegularExpression("[{},\\s]"));
        if (rows.empty() || !residue.isEmpty())
            fail("Invalid immediate constant buffer");
        globals.push_back("static const uint4 icb[" + std::to_string(rows.size()) + "] = {" +
                          join(rows, ",") + "};");
        return true;
    }
    auto m = match(R"(dcl_constantbuffer CB(\d+)\[(\d+)\], (?:immediateIndexed|dynamicIndexed))", line);
    if (!m.empty()) {
        globals.push_back("cbuffer CapturedCB" + m[1] + " : register(b" + m[1] + ") { uint4 cb" + m[1] + '[' +
                          m[2] + "]; }");
        return true;
    }
    m = match(
        R"(dcl_(resource|uav_typed)_(texture2d|texture2darray|texture3d|texturecube|texturecubearray|buffer) \((float|uint|sint),(?:float|uint|sint),(?:float|uint|sint),(?:float|uint|sint)\) ([tu]\d+))",
        line);
    if (!m.empty()) {
        const auto &access = m[1], &dimension = m[2], &name = m[4];
        const auto kind = m[3] == "sint" ? "int" : m[3];
        if (access == "uav_typed" && dimension.starts_with("texturecube"))
            fail("Cube UAVs are not supported by DX11");
        static const std::map<std::string, std::string> types{{"texture2d", "Texture2D"},
                                                              {"texture2darray", "Texture2DArray"},
                                                              {"texture3d", "Texture3D"},
                                                              {"texturecube", "TextureCube"},
                                                              {"texturecubearray", "TextureCubeArray"},
                                                              {"buffer", "Buffer"}};
        globals.push_back((access == "uav_typed" ? "RW" : "") + types.at(dimension) + '<' + kind +
                          (scalarUavs.contains(name) ? "" : "4") + "> " + name + " : register(" + name +
                          ");");
        resources[name] = {dimension, kind};
        return true;
    }
    m = match(R"(dcl_sampler (s\d+), mode_(default|comparison))", line);
    if (!m.empty()) {
        samplers[m[1]] = m[2];
        globals.push_back(std::string(m[2] == "comparison" ? "SamplerComparisonState" : "SamplerState") +
                          ' ' + m[1] + " : register(" + m[1] + ");");
        return true;
    }
    m = match(R"(dcl_temps (\d+))", line);
    if (!m.empty()) {
        temps = std::max(temps, number(m[1]));
        return true;
    }
    m = match(R"(dcl_indexableTemp (x\d+)\[(\d+)\], ([1-4]))", line);
    if (!m.empty()) {
        const auto count = number(m[2]);
        if (!count || count > 4096 ||
            std::any_of(indexableTemps.begin(), indexableTemps.end(),
                        [&](const auto &item) { return item.first == m[1]; }))
            fail("Invalid indexable temporary declaration");
        indexableTemps.push_back({m[1], {count, number(m[3])}});
        return true;
    }
    m = match(R"(dcl_thread_group (\d+), (\d+), (\d+))", line);
    if (!m.empty()) {
        threads = {number(m[1]), number(m[2]), number(m[3])};
        return true;
    }
    m = match(
        R"(dcl_input (vThreadID|vThreadGroupID|vThreadIDInGroup|vThreadIDInGroupFlattened)(?:\.[xyzw]+)?)",
        line);
    if (!m.empty()) {
        inputs.insert(m[1]);
        return true;
    }
    if (line == "dcl_globalFlags refactoringAllowed" ||
        line == "dcl_globalFlags refactoringAllowed | skipOptimization") {
        skipOptimization = line.find("skipOptimization") != std::string::npos;
        return true;
    }
    if (stage == "hs" || stage == "ds") {
        for (
            const auto pattern :
            {R"(dcl_(?:input|output)_control_point_count \d+)",
             R"(dcl_tessellator_(?:domain domain_tri|partitioning partitioning_(?:integer|pow2|fractional_even|fractional_odd)|output_primitive output_triangle_(?:cw|ccw)))",
             R"(dcl_hs_max_tessfactor l\((?:0x[0-9a-fA-F]{8}|[0-9.]+)\))",
             R"(dcl_input (?:vicp\[\d+\]\[\d+\]|vDomain)(?:\.[xyzw]+)?)",
             R"(dcl_output(?:_siv)? o\d+\.[xyzw]+(?:, \w+)?)"})
            if (!match(pattern, line).empty())
                return true;
    }
    return false;
}
std::string Lowerer::sampleOffset(const std::string &op, const std::string &dimension) {
    const auto m = match(R"(aoffimmi(?:_indexable)?\(([-\d]+),([-\d]+),([-\d]+)\))", op, false);
    if (m.empty())
        return "";
    std::array<int, 3> values{};
    for (unsigned i = 0; i < 3; ++i) {
        size_t end = 0;
        values[i] = std::stoi(m[i + 1], &end);
        if (end != m[i + 1].size())
            fail("Invalid sample offset");
    }
    if ((dimension != "texture2d" && dimension != "texture2darray" && dimension != "texture3d") ||
        (dimension != "texture3d" && values[2]))
        fail("Unsupported texture sample offset dimension");
    const auto width = dimension == "texture3d" ? 3 : 2;
    Strings offsets;
    for (int i = 0; i < width; ++i) {
        if (values[i] < -8 || values[i] > 7)
            fail("Texture sample offset outside DXBC range");
        offsets.push_back(std::to_string(values[i]));
    }
    return ",int" + std::to_string(width) + '(' + join(offsets, ",") + ')';
}
std::string Lowerer::instruction(const std::string &line) {
    const auto ins = parseInstruction(line);
    instructionPrecise = ins.precise;
    currentOpcode = ins.opcode;
    const bool sat = ins.opcode.ends_with("_sat");
    const auto op = sat ? ins.opcode.substr(0, ins.opcode.size() - 4) : ins.opcode;
    const auto &a = ins.arguments;
    auto f = [&](const std::string &s) { return raw(s, "f"); };
    auto i = [&](const std::string &s) { return raw(s, "i"); };
    auto u = [&](const std::string &s) { return raw(s, "u"); };
    auto b = [&](const std::string &s) { return raw(s); };
    if (auto code = memoryInstruction(op, a))
        return *code;
    if (auto code = geometryInstruction(op, a))
        return *code;
    if (op == "f16tof32")
        return write(a.at(0), "f16tof32(" + u(a.at(1)) + ')', "f", sat);
    if (op == "f32tof16")
        return write(a.at(0), "f32tof16(" + f(a.at(1)) + ')');
    static const std::map<std::string, std::string> derivatives{{"deriv_rtx", "ddx"},
                                                                {"deriv_rty", "ddy"},
                                                                {"deriv_rtx_coarse", "ddx_coarse"},
                                                                {"deriv_rty_coarse", "ddy_coarse"},
                                                                {"deriv_rtx_fine", "ddx_fine"},
                                                                {"deriv_rty_fine", "ddy_fine"}};
    if (derivatives.contains(op)) {
        if (stage != "ps")
            fail("Derivatives require a pixel shader");
        return write(a.at(0), derivatives.at(op) + '(' + f(a.at(1)) + ')', "f", sat);
    }
    if (op == "discard_z" || op == "discard_nz") {
        if (stage != "ps")
            fail("Discard requires a pixel shader");
        return "if ((" + u(a.at(0)) + ").x " + (op == "discard_z" ? "==" : "!=") + " 0u) discard;";
    }
    auto resourceOperand = [&](const std::string &s) {
        const auto dot = s.find('.');
        return std::make_pair(s.substr(0, dot), dot == std::string::npos ? std::string{} : s.substr(dot + 1));
    };
    static const std::map<std::string, std::string> textureCoords{
        {"texture2d", "xy"},    {"texture2darray", "xyz"},    {"texture3d", "xyz"},
        {"texturecube", "xyz"}, {"texturecubearray", "xyzw"}, {"buffer", "x"}};
    if (op == "sample" || op.starts_with("sample_indexable(") || op.starts_with("sample_aoffimmi") ||
        op.starts_with("sample_d_indexable(")) {
        const bool grad = op.starts_with("sample_d_");
        if (stage != "ps" && !grad)
            fail("Implicit texture sampling requires PS");
        const auto [resource, swizzle] = resourceOperand(a.at(2));
        const auto &[dimension, kind] = resources.at(resource);
        if (!textureCoords.contains(dimension) || dimension == "buffer" || kind != "float")
            fail("Unsupported filtered texture type");
        std::string extra;
        if (grad) {
            const auto width =
                dimension == "texture3d" || dimension == "texturecube" || dimension == "texturecubearray"
                    ? "xyz"
                    : "xy";
            extra = ",(" + f(a.at(4)) + ")." + width + ",(" + f(a.at(5)) + ")." + width;
        }
        auto expression = resource + '.' + (grad ? "SampleGrad" : "Sample") + '(' + a.at(3) + ",(" +
                          f(a.at(1)) + ")." + textureCoords.at(dimension) + extra +
                          sampleOffset(op, dimension) + ')';
        if (!swizzle.empty())
            expression = '(' + expression + ")." + swizzle;
        return write(a.at(0), expression, "f", sat);
    }
    if (op == "if_nz" || op == "if_z" || op == "breakc_nz" || op == "breakc_z" || op == "retc_nz" ||
        op == "retc_z") {
        const auto cond = '(' + u(a.at(0)) + ").x " + (op.ends_with("nz") ? "!=" : "==") + " 0u";
        return "if (" + cond +
               (op.starts_with("if") ? ") {" : ") " + (op.starts_with("break") ? "break;" : returnCode));
    }
    if (op == "else")
        return "} else {";
    if (op == "endif" || op == "endloop")
        return "}";
    if (op == "loop")
        return "while (true) {";
    if (op == "break")
        return "break;";
    if (op == "ret")
        return returnCode;
    if (op == "add" || op == "mul" || op == "div")
        return write(a.at(0),
                     '(' + f(a.at(1)) + ')' +
                         (op == "add"   ? "+"
                          : op == "mul" ? "*"
                                        : "/") +
                         '(' + f(a.at(2)) + ')',
                     "f", sat);
    if (op == "mad") {
        Strings values;
        for (size_t n = 1; n < a.size(); ++n)
            values.push_back(f(a[n]));
        return write(a.at(0), "mad(" + join(values, ",") + ')', "f", sat);
    }
    if (op == "min" || op == "max")
        return write(a.at(0), op + '(' + f(a.at(1)) + ',' + f(a.at(2)) + ')', "f", sat);
    static const std::map<std::string, std::string> unary{
        {"sqrt", "sqrt"},     {"rsq", "rsqrt"},     {"rcp", "rcp"},        {"frc", "frac"},
        {"exp", "exp2"},      {"log", "log2"},      {"round_ni", "floor"}, {"round_pi", "ceil"},
        {"round_z", "trunc"}, {"round_ne", "round"}};
    if (unary.contains(op))
        return write(a.at(0), unary.at(op) + '(' + f(a.at(1)) + ')', "f", sat);
    if (op == "dp2" || op == "dp3" || op == "dp4") {
        const auto mask = std::string("xyzw").substr(0, size_t(op.back() - '0'));
        return write(a.at(0),
                     "((float4)dot((" + f(a.at(1)) + ")." + mask + ",(" + f(a.at(2)) + ")." + mask + "))",
                     "f", sat);
    }
    if (op == "mov")
        return write(a.at(0), sat ? f(a.at(1)) : b(a.at(1)), sat ? "f" : "bits", sat);
    if (op == "movc")
        return write(a.at(0), "((" + u(a.at(1)) + ") != 0u ? " + b(a.at(2)) + " : " + b(a.at(3)) + ')');
    static const std::map<std::string, std::pair<std::string, std::string>> comparisons{
        {"eq", {"f", "=="}},  {"ne", {"f", "!="}},  {"lt", {"f", "<"}},  {"ge", {"f", ">="}},
        {"ieq", {"i", "=="}}, {"ine", {"i", "!="}}, {"ilt", {"i", "<"}}, {"ige", {"i", ">="}},
        {"ult", {"u", "<"}},  {"uge", {"u", ">="}}};
    if (comparisons.contains(op)) {
        const auto &[kind, symbol] = comparisons.at(op);
        return write(a.at(0),
                     "((" + raw(a.at(1), kind) + ") " + symbol + " (" + raw(a.at(2), kind) +
                         ") ? uint4(0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu) : uint4(0,0,0,0))");
    }
    static const std::map<std::string, std::pair<std::string, std::string>> binary{
        {"iadd", {"+", "i"}},  {"and", {"&", "u"}},   {"or", {"|", "u"}},   {"xor", {"^", "u"}},
        {"ishl", {"<<", "u"}}, {"ishr", {">>", "i"}}, {"ushr", {">>", "u"}}};
    if (binary.contains(op)) {
        const auto &[symbol, kind] = binary.at(op);
        const auto right =
            op == "ishl" || op == "ishr" || op == "ushr" ? '(' + u(a.at(2)) + " & 31u)" : raw(a.at(2), kind);
        return write(a.at(0), '(' + raw(a.at(1), kind) + ')' + symbol + '(' + right + ')', kind);
    }
    if (op == "not")
        return write(a.at(0), "~(" + u(a.at(1)) + ')');
    if (op == "imad" || op == "umad")
        return write(a.at(0), '(' + u(a.at(1)) + ") * (" + u(a.at(2)) + ") + (" + u(a.at(3)) + ')');
    if (op == "ineg")
        return write(a.at(0), "(0u - (" + u(a.at(1)) + "))");
    if (op == "imul" || op == "umul" || op == "udiv") {
        const auto name = "integer" + std::to_string(++serial);
        auto code = "uint4 " + name + "a=" + u(a.at(2)) + ", " + name + "b=" + u(a.at(3)) + ";\n";
        if (op == "udiv") {
            code += "uint4 " + name + "hi=(" + name + "b==0u ? 0xffffffffu : " + name + "a / max(" + name +
                    "b,1u));\n";
            code += "uint4 " + name + "lo=(" + name + "b==0u ? 0xffffffffu : " + name + "a % max(" + name +
                    "b,1u));\n";
        } else {
            code += "uint4 " + name + "lo=" + name + "a * " + name + "b;\n";
            if (a.at(0) != "null") {
                code += "uint4 " + name + "al=" + name + "a & 65535u, " + name + "bl=" + name +
                        "b & 65535u, " + name + "ah=" + name + "a >> 16u, " + name + "bh=" + name +
                        "b >> 16u;\n";
                code += "uint4 " + name + "mid=(" + name + "al * " + name + "bl >> 16u) + " + name + "ah * " +
                        name + "bl;\n";
                code +=
                    "uint4 " + name + "mid2=(" + name + "mid & 65535u) + " + name + "al * " + name + "bh;\n";
                code += "uint4 " + name + "hi=" + name + "ah * " + name + "bh + (" + name +
                        "mid >> 16u) + (" + name + "mid2 >> 16u);\n";
                if (op == "imul")
                    code += name + "hi -= (" + name + "a >> 31u) * " + name + "b + (" + name +
                            "b >> 31u) * " + name + "a;\n";
            }
        }
        const auto high = write(a.at(0), name + "hi");
        const auto low = write(a.at(1), name + "lo");
        return code + high + '\n' + low;
    }
    if (op == "ubfe" || op == "ibfe" || op == "bfi") {
        const auto name = "field" + std::to_string(++serial);
        auto code = "uint4 " + name + "w=" + u(a.at(1)) + " & 31u, " + name + "o=" + u(a.at(2)) + " & 31u, " +
                    name + "v=" + u(a.at(3)) + ";\n";
        std::string expression;
        if (op == "bfi") {
            code += "uint4 " + name + "mask=((1u << " + name + "w)-1u) << " + name + "o;\n";
            expression = "((" + name + "v << " + name + "o) & " + name + "mask) | (" + u(a.at(4)) + " & ~" +
                         name + "mask)";
        } else {
            auto shifted = '(' + name + "v << ((32u-" + name + "w-" + name + "o) & 31u))";
            if (op == "ibfe")
                shifted = "asint(" + shifted + ')';
            auto direct = (op == "ibfe" ? "asint(" + name + "v)" : name + 'v') + " >> " + name + 'o';
            expression = '(' + name + "w==0u ? 0 : (" + name + "w+" + name + "o < 32u ? (" + shifted +
                         " >> ((32u-" + name + "w) & 31u)) : (" + direct + ")))";
            if (op == "ibfe")
                expression = "asuint(" + expression + ')';
        }
        return code + write(a.at(0), expression);
    }
    if (op == "imin" || op == "imax" || op == "umin" || op == "umax") {
        const std::string kind(1, op[0]);
        return write(a.at(0), op.substr(1) + '(' + raw(a.at(1), kind) + ',' + raw(a.at(2), kind) + ')', kind);
    }
    if (op == "utof" || op == "itof" || op == "ftou" || op == "ftoi") {
        const std::string kind(1, op.back());
        return write(a.at(0),
                     std::string(kind == "f"   ? "float4"
                                 : kind == "i" ? "int4"
                                               : "uint4") +
                         '(' + raw(a.at(1), std::string(1, op[0])) + ')',
                     kind);
    }
    if (op == "sincos") {
        const auto name = "trig" + std::to_string(++serial);
        const auto sine = write(a.at(0), name + 's', "f");
        const auto cosine = write(a.at(1), name + 'c', "f");
        return "float4 " + name + "s," + name + "c; sincos(" + f(a.at(2)) + ',' + name + "s," + name +
               "c);\n" + sine + '\n' + cosine;
    }
    if (!match(R"(^gather4(?:_c)?(?:_aoffimmi)?_indexable\()", op, false).empty()) {
        const auto [resource, swizzle] = resourceOperand(a.at(2));
        const auto &[dimension, kind] = resources.at(resource);
        if (dimension != "texture2d" && dimension != "texture2darray" && dimension != "texturecube" &&
            dimension != "texturecubearray")
            fail("Unsupported gather texture dimension");
        const auto [sampler, component] = resourceOperand(a.at(3));
        if (component.size() != 1 || component.find_first_not_of("xyzw") != std::string::npos)
            fail("Missing gather component selector");
        const bool comparison = op.starts_with("gather4_c_");
        if (!samplers.contains(sampler) || samplers.at(sampler) != (comparison ? "comparison" : "default"))
            fail("Gather sampler kind mismatch");
        if (comparison && (component != "x" || kind != "float"))
            fail("Comparison gather requires the red float component");
        static const std::map<std::string, std::string> methods{
            {"x", "GatherRed"}, {"y", "GatherGreen"}, {"z", "GatherBlue"}, {"w", "GatherAlpha"}};
        const auto method = comparison ? "GatherCmp" : methods.at(component);
        const auto extra = comparison ? ",(" + f(a.at(4)) + ").x" : "";
        auto value = resource + '.' + method + '(' + sampler + ",(" + f(a.at(1)) + ")." +
                     textureCoords.at(dimension) + extra + sampleOffset(op, dimension) + ')';
        if (kind != "uint")
            value = "asuint(" + value + ')';
        if (!swizzle.empty())
            value = '(' + value + ")." + swizzle;
        return write(a.at(0), value);
    }
    if (op.starts_with("sample_c_lz_indexable(") || op.starts_with("sample_c_lz_aoffimmi_indexable(")) {
        const auto [resource, swizzle] = resourceOperand(a.at(2));
        const auto &[dimension, kind] = resources.at(resource);
        const auto &sampler = a.at(3);
        if (kind != "float" || (swizzle != "x" && swizzle != "xxxx") || !samplers.contains(sampler) ||
            samplers.at(sampler) != "comparison")
            fail("Comparison sample requires red float data and a comparison sampler");
        if (!textureCoords.contains(dimension) || dimension == "buffer" || dimension == "texture3d")
            fail("Unsupported comparison sample dimension");
        const auto expression = resource + ".SampleCmpLevelZero(" + sampler + ",(" + f(a.at(1)) + ")." +
                                textureCoords.at(dimension) + ",(" + f(a.at(4)) + ").x" +
                                sampleOffset(op, dimension) + ')';
        return write(a.at(0), "((float4)(" + expression + "))", "f", sat);
    }
    if (op.starts_with("ld_indexable(") || op.starts_with("ld_uav_typed_indexable(") ||
        op.starts_with("sample_l_") || op.starts_with("sample_l_indexable(")) {
        const auto [resource, swizzle] = resourceOperand(a.at(2));
        const auto &[dimension, kind] = resources.at(resource);
        const auto &coords = textureCoords.at(dimension);
        std::string expression;
        if (op.starts_with("ld_uav_"))
            expression = resource + "[(" + i(a.at(1)) + ")." + coords + ']';
        else if (op.starts_with("ld_")) {
            if (dimension.starts_with("texturecube"))
                fail("Cube textures do not support Load");
            static const std::map<std::string, std::string> masks{
                {"texture2d", "xyw"}, {"texture2darray", "xyzw"}, {"texture3d", "xyzw"}, {"buffer", "x"}};
            expression = resource + ".Load((" + i(a.at(1)) + ")." + masks.at(dimension) + ')';
        } else {
            const auto sampler = resourceOperand(a.at(3)).first;
            expression = resource + ".SampleLevel(" + sampler + ",(" + f(a.at(1)) + ")." + coords + ",(" +
                         f(a.at(4)) + ").x" + sampleOffset(op, dimension) + ')';
        }
        auto bits = kind != "uint" ? "asuint(" + expression + ')' : expression;
        if (scalarUavs.contains(resource)) {
            const auto sourceMask = swizzle.empty() ? "xyzw" : swizzle;
            const auto destMask = a.at(0).substr(a.at(0).rfind('.') + 1);
            for (auto lane : destMask)
                if (sourceMask.at(std::string("xyzw").find(lane)) != 'x')
                    fail("Non-x component of scalar atomic UAV load");
            bits = "((uint4)(" + bits + "))";
        }
        if (!swizzle.empty())
            bits = '(' + bits + ")." + swizzle;
        return write(a.at(0), bits);
    }
    if (op == "store_uav_typed") {
        const auto [resource, mask] = resourceOperand(a.at(0));
        const auto &[dimension, kind] = resources.at(resource);
        if (mask != "xyzw")
            fail("Partial UAV write mask unsupported");
        static const std::map<std::string, std::string> coords{
            {"texture2d", "xy"}, {"texture2darray", "xyz"}, {"texture3d", "xyz"}, {"buffer", "x"}};
        auto value = kind == "float" ? f(a.at(2)) : kind == "int" ? i(a.at(2)) : u(a.at(2));
        if (scalarUavs.contains(resource))
            value = '(' + value + ").x";
        return resource + "[(" + i(a.at(1)) + ")." + coords.at(dimension) + "] = " + value + ';';
    }
    fail("Unsupported DXBC instruction: " + line);
}
} // namespace flora::hlsl
