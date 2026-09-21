#include "HlslRecoveryInternal.h"
#include <QRegularExpression>

namespace flora::hlsl {
Strings Lowerer::prepareMemory(const Strings &body) const {
    Strings result;
    std::vector<std::pair<size_t, std::string>> discarded;
    for (size_t n = 0; n < body.size();) {
        const auto ins = parseInstruction(body[n]);
        const auto &a = ins.arguments;
        if (ins.opcode == "imm_atomic_alloc" && appendUavs.contains(a.at(1))) {
            if (n + 1 >= body.size())
                fail("Append allocation without a store");
            const auto next = parseInstruction(body[n + 1]);
            const auto &b = next.arguments;
            const auto &name = a[1];
            const auto stride = buffers.at(name).second;
            const auto mask = std::string("xyzw").substr(0, stride / 4);
            if ((stride != 4 && stride != 8 && stride != 12 && stride != 16) ||
                next.opcode != "store_structured" || b.size() < 4 || b[0] != name + '.' + mask ||
                b[1] != a.at(0) || b[2] != "l(0x00000000)")
                fail("Only adjacent full-element Append stores are recovered");
            result.push_back("flora_append " + name + ", " + b[3]);
            discarded.emplace_back(result.size() - 1, a[0]);
            n += 2;
        } else {
            result.push_back(body[n]);
            ++n;
        }
    }
    const QRegularExpression uses(QStringLiteral(R"(\b(r\d+)\.([xyzw]+))"));
    for (const auto &[start, destination] : discarded) {
        const auto dot = destination.find('.');
        if (dot == std::string::npos)
            fail("Invalid Append destination");
        const auto reg = destination.substr(0, dot), lane = destination.substr(dot + 1);
        for (size_t n = start; n < result.size(); ++n) {
            const auto ins = parseInstruction(result[n]);
            const auto &op = ins.opcode;
            const size_t first = op.starts_with("if_") || op.starts_with("store_") ||
                                         op.starts_with("atomic_") || op.starts_with("flora_")
                                     ? 0
                                     : 1;
            for (size_t i = first; i < ins.arguments.size(); ++i) {
                auto matches = uses.globalMatch(QString::fromStdString(ins.arguments[i]));
                while (matches.hasNext()) {
                    const auto m = matches.next();
                    if (m.captured(1).toStdString() == reg &&
                        m.captured(2).toStdString().find(lane) != std::string::npos)
                        fail("Append index escapes its paired store");
                }
            }
        }
    }
    return result;
}
bool Lowerer::memoryDeclaration(std::string line) {
    auto m = match(R"(dcl_uav_structured_opc (u\d+), \d+)", line);
    if (!m.empty())
        counterUavs.insert(m[1]);
    line = replace(line, "dcl_uav_structured_opc ", "dcl_uav_structured ");
    m = match(R"(dcl_tgsm_(raw|structured) (g\d+), (\d+)(?:, (\d+))?)", line);
    if (!m.empty()) {
        const auto &kind = m[1], &name = m[2];
        const auto size = number(m[3]);
        if (!size || size % 4 || (kind == "structured") != !m[4].empty())
            fail("Invalid TGSM layout");
        const uint64_t total = uint64_t(size) * (m[4].empty() ? 1 : number(m[4]));
        if (!total || total > 32768)
            fail("Unsupported TGSM size");
        buffers[name] = {"shared_" + kind, size};
        globals.push_back("groupshared uint " + name + '[' + std::to_string(total / 4) + "];");
        return true;
    }
    m = match(R"(dcl_(resource|uav)_(raw|structured) ([tu]\d+)(?:, (\d+))?)", line);
    if (m.empty())
        return false;
    const auto &access = m[1], &kind = m[2], &name = m[3];
    if ((kind == "structured") != !m[4].empty())
        fail("Invalid buffer declaration");
    const auto size = m[4].empty() ? 0 : number(m[4]);
    if (kind == "structured" && (!size || size % 4 || size > 2048))
        fail("Invalid structured buffer stride");
    buffers[name] = {kind, size};
    if (consumeUavs.contains(name))
        fail("ConsumeStructuredBuffer recovery is not implemented");
    std::string type;
    if (kind == "structured") {
        const auto element = "CapturedElement_" + name;
        globals.push_back("struct " + element + " { uint words[" + std::to_string(size / 4) + "]; };");
        type = "StructuredBuffer<" + element + '>';
    } else
        type = "ByteAddressBuffer";
    const auto prefix = appendUavs.contains(name) ? "Append" : access == "uav" ? "RW" : "";
    globals.push_back(prefix + type + ' ' + name + " : register(" + name + ");");
    return true;
}
std::string Lowerer::memoryWord(const std::string &name, const std::string &index, const std::string &offset,
                                unsigned lane) const {
    const auto &[kind, stride] = buffers.at(name);
    const auto word = "((" + offset + ") >> 2u) + " + std::to_string(lane) + 'u';
    if (kind == "shared_structured")
        return name + "[(" + index + ") * " + std::to_string(stride / 4) + "u + " + word + ']';
    if (kind == "shared_raw")
        return name + '[' + word + ']';
    if (kind == "structured")
        return name + '[' + index + "].words[" + word + ']';
    return name + ".Load((" + offset + ") + " + std::to_string(lane * 4) + "u)";
}
std::optional<std::string> Lowerer::memoryInstruction(const std::string &op, const Strings &a) {
    auto u = [&](const std::string &s) { return raw(s, "u"); };
    auto scalar = [&](const std::string &s) { return '(' + u(s) + ").x"; };
    static const std::map<std::string, std::string> barriers{
        {"sync_g", "GroupMemoryBarrier"},        {"sync_g_t", "GroupMemoryBarrierWithGroupSync"},
        {"sync_uglobal", "DeviceMemoryBarrier"}, {"sync_uglobal_t", "DeviceMemoryBarrierWithGroupSync"},
        {"sync_uglobal_g", "AllMemoryBarrier"},  {"sync_uglobal_g_t", "AllMemoryBarrierWithGroupSync"}};
    if (barriers.contains(op))
        return barriers.at(op) + "();";
    if (op == "flora_append") {
        const auto &name = a.at(0);
        const auto temp = "append" + std::to_string(++serial);
        const auto count = buffers.at(name).second / 4;
        std::string code =
            "CapturedElement_" + name + ' ' + temp + "; uint4 " + temp + "v=" + u(a.at(1)) + ";\n";
        Strings words;
        for (unsigned n = 0; n < count; ++n) {
            if (n >= 4)
                fail("Unsupported Append element width");
            words.push_back(temp + ".words[" + std::to_string(n) + "]=" + temp + "v." + "xyzw"[n] + ';');
        }
        return code + join(words, "\n") + '\n' + name + ".Append(" + temp + ");";
    }
    if (op == "imm_atomic_alloc" || op == "imm_atomic_consume") {
        const auto &name = a.at(1);
        if (appendUavs.contains(name))
            fail("Unpaired Append operation");
        if (!counterUavs.contains(name))
            fail("Hidden counter kind requires reflection or an OPC declaration");
        if (!name.starts_with('u') || !buffers.contains(name) || buffers.at(name).first != "structured")
            fail("Counter operation requires a structured UAV");
        const auto temp = "counter" + std::to_string(++serial);
        return "uint " + temp + '=' + name + '.' +
               (op == "imm_atomic_alloc" ? "IncrementCounter" : "DecrementCounter") + "();\n" +
               write(a.at(0), "((uint4)" + temp + ')');
    }
    const bool structured = op == "ld_structured" || op == "store_structured" ||
                            op.starts_with("ld_structured_indexable(") ||
                            op.starts_with("store_structured_indexable(");
    const bool isRaw = op == "ld_raw" || op == "store_raw" || op.starts_with("ld_raw_indexable(") ||
                       op.starts_with("store_raw_indexable(");
    if (structured || isRaw) {
        const bool load = op.starts_with("ld_");
        const auto resource = load ? a.back() : a.at(0);
        const auto dot = resource.find('.');
        const auto name = resource.substr(0, dot);
        auto mask = dot == std::string::npos ? "" : resource.substr(dot + 1);
        if (appendUavs.contains(name))
            fail("Unpaired Append memory access");
        if (!buffers.contains(name))
            fail("Memory instruction/resource layout mismatch: " + name);
        const auto &kind = buffers.at(name).first;
        if (structured ? kind != "structured" && kind != "shared_structured"
                       : kind != "raw" && kind != "shared_raw")
            fail("Memory instruction/resource layout mismatch: " + name);
        if (!load && name.starts_with('t'))
            fail("Cannot write SRV memory");
        const auto index = structured ? scalar(a.at(1)) : "0u";
        const auto offset = scalar(a.at(structured ? 2 : 1));
        if (load) {
            if (mask.empty())
                mask = "xyzw";
            if (mask.size() == 1)
                mask.resize(4, mask[0]);
            if (mask.size() != 4)
                fail("Invalid memory load swizzle");
            const auto &dest = a.at(0);
            const auto destDot = dest.rfind('.');
            if (destDot == std::string::npos)
                fail("Invalid memory load destination");
            const auto destMask = dest.substr(destDot + 1);
            Strings values;
            for (unsigned n = 0; n < 4; ++n) {
                if (destMask.find("xyzw"[n]) == std::string::npos)
                    values.push_back("0u");
                else {
                    const auto lane = std::string("xyzw").find(mask[n]);
                    if (lane == std::string::npos)
                        fail("Invalid memory load swizzle");
                    values.push_back(memoryWord(name, index, offset, unsigned(lane)));
                }
            }
            return write(dest, "uint4(" + join(values, ",") + ')');
        }
        if (mask.empty() || mask.find_first_not_of("xyzw") != std::string::npos)
            fail("Invalid memory store mask");
        const auto temp = "store" + std::to_string(++serial);
        std::string code = "uint " + temp + "i=" + index + ", " + temp + "o=" + offset + "; uint4 " + temp +
                           "v=" + u(a.back()) + ";\n";
        for (auto lane : mask) {
            const auto component = unsigned(std::string("xyzw").find(lane));
            if (kind == "raw")
                code += name + ".Store(" + temp + "o + " + std::to_string(component * 4) + "u, " + temp +
                        "v." + lane + ");\n";
            else
                code +=
                    memoryWord(name, temp + 'i', temp + 'o', component) + " = " + temp + "v." + lane + ";\n";
        }
        return code;
    }
    if (op == "imm_atomic_iadd" || op == "atomic_iadd") {
        const bool returning = op.starts_with("imm_");
        const auto &resource = a.at(returning ? 1 : 0), &address = a.at(returning ? 2 : 1),
                   &value = a.at(returning ? 3 : 2);
        if (resource.starts_with('t'))
            fail("Atomic on an SRV");
        std::optional<std::string> target;
        if (buffers.contains(resource)) {
            const auto &kind = buffers.at(resource).first;
            if (kind.ends_with("structured"))
                target = memoryWord(resource, scalar(address), '(' + u(address) + ").y", 0);
            else if (kind == "shared_raw")
                target = memoryWord(resource, "0u", scalar(address), 0);
        } else if (resources.contains(resource)) {
            const auto &[dimension, kind] = resources.at(resource);
            if (kind != "uint")
                fail("Only unsigned typed UAV atomic lowering is implemented");
            static const std::map<std::string, std::string> coords{
                {"buffer", "x"}, {"texture2d", "xy"}, {"texture2darray", "xyz"}};
            target = resource + "[(" + u(address) + ")." + coords.at(dimension) + ']';
        } else
            fail("Unknown atomic resource " + resource);
        const auto name = "atomic" + std::to_string(++serial);
        const auto call = target ? "InterlockedAdd(" + *target + ", " + scalar(value) + ", " + name + ");"
                                 : resource + ".InterlockedAdd(" + scalar(address) + ", " + scalar(value) +
                                       ", " + name + ");";
        return "uint " + name + "; " + call +
               (returning
                    ? '\n' + write(a.at(0), "uint4(" + name + ',' + name + ',' + name + ',' + name + ')')
                    : "");
    }
    return std::nullopt;
}
} // namespace flora::hlsl
