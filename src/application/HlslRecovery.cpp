#include "HlslRecoveryInternal.h"
#include "ShaderInspector.h"
#include "SystemDisassembly.h"
#include <QRegularExpression>
#include <QString>

namespace flora::hlsl {
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error(message); }
Strings match(const std::string &pattern, const std::string &text, bool full) {
    thread_local std::map<std::pair<std::string, bool>, QRegularExpression> cache;
    const auto key = std::make_pair(pattern, full);
    auto it = cache.find(key);
    if (it == cache.end()) {
        QRegularExpression regex(QString::fromStdString(full ? "\\A(?:" + pattern + ")\\z" : pattern),
                                 QRegularExpression::UseUnicodePropertiesOption);
        if (!regex.isValid())
            fail("Invalid native HLSL recovery pattern");
        it = cache.emplace(key, std::move(regex)).first;
    }
    const auto found = it->second.match(QString::fromStdString(text));
    Strings result;
    if (found.hasMatch())
        // capturedTexts() omits unmatched trailing groups. Keep every declared
        // group so optional swizzles, strides and TGSM counts have stable indices.
        for (int i = 0; i <= it->second.captureCount(); ++i)
            result.push_back(found.captured(i).toStdString());
    return result;
}
std::string strip(const std::string &value) {
    auto s = QString::fromStdString(value);
    auto whitespace = [](QChar c) { return c.isSpace() || (c.unicode() >= 0x1c && c.unicode() <= 0x1f); };
    qsizetype start = 0, end = s.size();
    while (start < end && whitespace(s[start]))
        ++start;
    while (end > start && whitespace(s[end - 1]))
        --end;
    return s.mid(start, end - start).toStdString();
}
std::string join(const Strings &values, const std::string &separator) {
    std::string result;
    bool first = true;
    for (const auto &s : values) {
        if (!first)
            result += separator;
        first = false;
        result += s;
    }
    return result;
}
std::string replace(std::string value, const std::string &before, const std::string &after) {
    size_t pos = 0;
    while ((pos = value.find(before, pos)) != std::string::npos) {
        value.replace(pos, before.size(), after);
        pos += after.size();
    }
    return value;
}
uint32_t number(const std::string &value) {
    bool ok = false;
    const auto n = QString::fromStdString(value).toULongLong(&ok);
    if (!ok || n > UINT32_MAX)
        fail("DXBC declaration integer exceeds uint32");
    return uint32_t(n);
}
Strings splitArguments(const std::string &value) {
    Strings result;
    size_t start = 0;
    int depth = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        const auto c = value[i];
        if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;
        else if (c == ',' && !depth) {
            result.push_back(strip(value.substr(start, i - start)));
            start = i + 1;
        }
    }
    result.push_back(strip(value.substr(start)));
    return result;
}
Instruction parseInstruction(const std::string &line) {
    // Work in Unicode code points so Python's whitespace split semantics survive.
    const auto s = QString::fromStdString(line);
    int depth = 0;
    qsizetype end = s.size();
    for (qsizetype n = 0; n < s.size(); ++n) {
        if (s[n] == '(')
            ++depth;
        else if (s[n] == ')')
            --depth;
        else if ((s[n].isSpace() || (s[n].unicode() >= 0x1c && s[n].unicode() <= 0x1f)) && !depth) {
            end = n;
            break;
        }
    }
    Instruction out;
    out.opcode = s.left(end).toStdString();
    auto rest = strip(s.mid(end).toStdString());
    auto precise = match(R"(^\[precise(?:\([xyzw]+\))?\]\s*)", rest, false);
    if (!precise.empty()) {
        out.precise = true;
        rest.erase(0, precise[0].size());
        const auto suffix = match(R"(^((?:\([^)]*\))+)\s+)", rest, false);
        if (!suffix.empty()) {
            out.opcode += suffix[1];
            rest.erase(0, suffix[0].size());
        }
    }
    if (!rest.empty())
        out.arguments = splitArguments(rest);
    return out;
}
Lowerer::Lowerer(Json metadata, const std::string &assembly) : info(std::move(metadata)) {
    stage = info.at("stage").get<std::string>();
    if (!std::set<std::string>{"vs", "ps", "gs", "cs", "hs", "ds"}.contains(stage))
        fail("Unsupported shader stage");
    std::string pending;
    for (const auto &part : QString::fromStdString(assembly).split(
             QRegularExpression("\\r\\n|[\\n\\r\\v\\f\\x{001c}-\\x{001e}\\x{0085}\\x{2028}\\x{2029}]"))) {
        const auto line = strip(part.toStdString());
        if (line.empty() || line.starts_with("//"))
            continue;
        if (!pending.empty() || line.starts_with("dcl_immediateConstantBuffer")) {
            pending += " " + line;
            if (std::count(pending.begin(), pending.end(), '{') ==
                std::count(pending.begin(), pending.end(), '}')) {
                lines.push_back(strip(pending));
                pending.clear();
            }
        } else
            lines.push_back(line);
    }
    if (!pending.empty())
        fail("Unclosed immediate constant buffer");
    for (const auto &line : lines) {
        const auto ins = parseInstruction(line);
        if (ins.opcode == "imm_atomic_iadd" || ins.opcode == "atomic_iadd") {
            const auto &resource = ins.arguments.at(ins.opcode.starts_with("imm_") ? 1 : 0);
            if (resource.starts_with("u"))
                scalarUavs.insert(resource);
        }
    }
    for (const auto &binding : info.at("bindings")) {
        const auto name = "u" + std::to_string(binding.at("slot").get<uint32_t>());
        const auto type = binding.at("type").get<unsigned>();
        if (type == 9)
            appendUavs.insert(name);
        if (type == 10)
            consumeUavs.insert(name);
        if (type == 11)
            counterUavs.insert(name);
    }
}
std::string Lowerer::raw(std::string operand, const std::string &kind) const {
    const bool negative = operand.starts_with('-');
    if (negative)
        operand.erase(0, 1);
    const bool absolute = operand.starts_with('|') && operand.ends_with('|');
    if (absolute)
        operand = operand.substr(1, operand.size() - 2);
    std::string value;
    if (operand.starts_with("l(") && operand.ends_with(')')) {
        auto values = splitArguments(operand.substr(2, operand.size() - 3));
        if (values.size() == 1)
            values.resize(4, values[0]);
        if (values.size() != 4)
            fail("Expected exact hexadecimal immediate: " + operand);
        for (auto &v : values) {
            if (match("0x[0-9a-fA-F]{8}", v).empty())
                fail("Expected exact hexadecimal immediate: " + operand);
            v += 'u';
        }
        value = "uint4(" + join(values, ",") + ")";
    } else {
        const auto parsed = match(R"((.+?)(?:\.([xyzw]{1,4}))?)", operand);
        if (parsed.empty())
            fail("Unsupported operand: " + operand);
        const auto &base = parsed[1];
        auto swizzle = parsed[2];
        if (match(
                R"((?:r\d+|o\d+|v\d+|vPrim|vGSInstanceID|v\[[r0-9xyzw.+ *\-]+\]\[\d+\]|vThreadID|vThreadGroupID|vThreadIDInGroup|vThreadIDInGroupFlattened|vDomain|(?:cb\d+|icb|x\d+)\[[r0-9xyzw.+ *\-]+\]|vicp\[\d+\]\[\d+\]))",
                base)
                .empty())
            fail("Unsupported operand: " + operand);
        if (!swizzle.empty()) {
            if (swizzle.size() == 1)
                swizzle.resize(4, swizzle[0]);
            else if (swizzle.size() != 4)
                fail("Non-scalar source needs four swizzle lanes: " + operand);
            value = base + '.' + swizzle;
        } else
            value = base;
    }
    if (kind == "f" || kind == "i") {
        value = (kind == "f" ? "asfloat(" : "asint(") + value + ')';
        if (absolute)
            value = "abs(" + value + ')';
        if (negative)
            value = "(-" + value + ')';
    } else {
        if (absolute)
            value = '(' + value + " & 0x7fffffffu)";
        if (negative)
            value = '(' + value + (kind == "bits" ? " ^ 0x80000000u)" : " * 0xffffffffu)");
    }
    return value;
}
std::string Lowerer::write(std::string dest, std::string expression, const std::string &kind, bool saturate) {
    if (dest == "null")
        return "";
    if (dest == "oDepth" || dest == "oDepthGE" || dest == "oDepthLE")
        dest += ".x";
    const auto parsed = match(R"(([ro]\d+|oDepth(?:GE|LE)?|x\d+\[[r0-9xyzw.+ *\-]+\])\.([xyzw]{1,4}))", dest);
    if (parsed.empty())
        fail("Unsupported destination: " + dest);
    if (saturate) {
        if (kind != "f")
            fail("Saturate requires float operation");
        expression = "saturate(" + expression + ')';
    }
    std::string prefix;
    const bool ordered = stage == "ps" ||
                         (stage == "ds" && (expression.find("vDomain") != std::string::npos ||
                                            expression.find("vicp") != std::string::npos)) ||
                         orderedSampleMad || instructionPrecise ||
                         (stage == "vs" && (currentOpcode == "mad" || currentOpcode == "mad_sat"));
    if (kind == "f" && ordered) {
        const auto name = "value" + std::to_string(++serial);
        prefix = "precise float4 " + name + " = " + expression + ";\n";
        expression = name;
    }
    if (kind == "f" || kind == "i")
        expression = "asuint(" + expression + ')';
    return prefix + parsed[1] + '.' + parsed[2] + " = (" + expression + ")." + parsed[2] + ';';
}
} // namespace flora::hlsl
