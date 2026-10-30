#include "DxbcGeometryDependencies.h"
#include "DxbcCheckpointModel.h"
namespace flora {
namespace {
using Words = std::vector<uint32_t>;
// A straight-line GS has a value-independent emission/cut schedule. Unknown
// instructions and every branch, loop, call or early return keep the old proof.
nlohmann::json fixedGeometrySchedule(const DxbcProgram &program) {
    nlohmann::json schedule = nlohmann::json::array();
    uint32_t token = 2;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        const auto &row = program.instructions[i];
        const auto op = row.at(0) & 2047;
        if (op == 62) {
            if (i + 1 != program.instructions.size() || row.size() != 1)
                return nullptr;
            return {{"operations", schedule}, {"return_token", token}};
        }
        if (op == 9 || op == 19 || op == 20 || (op >= 117 && op <= 119)) {
            const bool explicitStream = op >= 117;
            if (explicitStream ? (row.size() != 3 || row[1] != 0x110000 || row[2] > 3) : row.size() != 1)
                return nullptr;
            schedule.push_back({{"token", token}, {"opcode", op}, {"stream", explicitStream ? row[2] : 0u}});
        } else {
            const bool declaration = (op >= 88 && op <= 106) || op == 143 || (op >= 147 && op <= 158) ||
                                     op == 161 || op == 162 || op == 206;
            const bool value = op <= 1 || (op >= 14 && op <= 17) || (op >= 24 && op <= 30) ||
                               (op >= 32 && op <= 43) || (op >= 45 && op <= 47) || (op >= 49 && op <= 60) ||
                               (op >= 64 && op <= 75) || (op >= 77 && op <= 87) || (op >= 129 && op <= 142) ||
                               (op >= 163 && op <= 189);
            if (!declaration && !value)
                return nullptr;
        }
        token += uint32_t(row.size());
    }
    return nullptr;
}
// Inspect actual operand trees, never immediate payload words that resemble tokens.
size_t visitOperand(const Words &words, size_t pos, const std::function<void(uint32_t, size_t, bool)> &visit,
                    bool nested = false) {
    const auto token = words.at(pos++), kind = (token >> 12) & 255;
    const auto start = pos - 1;
    for (auto extended = token >> 31; extended;)
        extended = words.at(pos++) >> 31;
    visit(token, start, nested);
    for (unsigned i = 0; i < ((token >> 20) & 3); ++i) {
        const auto mode = (token >> (22 + 3 * i)) & 7;
        if (mode == 0 || mode == 3)
            ++pos;
        else if (mode == 1 || mode == 4)
            pos += 2;
        else if (mode != 2)
            throw std::runtime_error("Unsupported operand index");
        if (mode >= 2)
            pos = visitOperand(words, pos, visit, true);
    }
    if (kind == 4 || kind == 5)
        pos += (token & 3) == 1 ? 1 : 4;
    return pos;
}
bool deadAtomicResult(const DxbcProgram &program, size_t at, size_t begin, size_t end, uint64_t &budget) {
    const auto &origin = program.instructions[at];
    if (end == begin + 1 && origin[begin] == 0x0000d000u)
        return true;
    const auto token = origin[begin];
    if (end != begin + 2 || (token & ~0xf0u) != 0x00100002u || !(token & 0xf0u))
        return false;
    const auto temporary = origin[begin + 1];
    auto live = (token >> 4) & 15;
    for (size_t i = at + 1; i < program.instructions.size(); ++i) {
        if (!budget)
            return false;
        --budget;
        const auto &row = program.instructions[i];
        const auto op = row[0] & 2047;
        // No control-flow assumptions: an unoverwritten result may only die at
        // the final return. Other phase boundaries, branches and calls fail.
        if (op == 62)
            return i + 1 == program.instructions.size();
        const bool alu = op <= 1 || (op >= 14 && op <= 17) || (op >= 24 && op <= 30) ||
                         (op >= 32 && op <= 43) || op == 47 || (op >= 49 && op <= 57) || op == 59 ||
                         op == 60 || (op >= 64 && op <= 68) || op == 75 || (op >= 77 && op <= 87) ||
                         (op >= 129 && op <= 142);
        const bool store = op == 164 || op == 166 || op == 168 || (op >= 169 && op <= 177);
        if (!alu && !store && op != 58 && op != 9 && op != 19 && op != 20)
            return false;
        const auto operands = checkpoint::instructionOperands(row);
        const size_t destinations =
            !alu                                                                                    ? 0
            : (op == 38 || op == 77 || op == 78 || op == 81 || op == 132 || op == 133 || op == 142) ? 2
                                                                                                    : 1;
        auto killed = 0u;
        bool read = false;
        for (size_t index = 0; index < operands.ranges.size(); ++index) {
            const auto [a, b] = operands.ranges[index];
            visitOperand(row, a, [&](uint32_t operand, size_t word, bool nested) {
                if (((operand >> 12) & 255) != 0)
                    return;
                if ((operand >> 31) || ((operand >> 20) & 3) != 1 || ((operand >> 22) & 7)) {
                    read = true;
                    return;
                }
                if (row.at(word + 1) != temporary)
                    return;
                if (!nested && index < destinations && (operand & ~0xf0u) == 0x00100002u)
                    killed |= (operand >> 4) & 15;
                else {
                    const auto mode = (operand >> 2) & 3;
                    uint32_t used = 15;
                    if ((operand & 3) == 2 && mode == 2)
                        used = 1u << ((operand >> 4) & 3);
                    else if ((operand & 3) == 2 && mode == 1) {
                        used = 0;
                        for (unsigned c = 0; c < 4; ++c)
                            used |= 1u << ((operand >> (4 + c * 2)) & 3);
                    }
                    read |= bool(used & live);
                }
            });
        }
        if (read)
            return false;
        live &= ~killed;
        if (!live)
            return true;
    }
    return false;
}
} // namespace
nlohmann::json geometryUavIndependence(Bytes bytecode, bool terminalDomainCounts,
                                       bool terminalGeometryCounts) {
    using Json = nlohmann::json;
    Json result{{"schema", 1},
                {"shader_sha256", sha256(bytecode)},
                {"proved", false},
                {"scope", "no_uav_results_or_srv_reads"},
                {"terminal_domain_counts", terminalDomainCounts},
                {"terminal_geometry_counts", false},
                {"write_instructions", Json::array()}};
    auto reject = [&](const char *reason) {
        result["reason"] = reason;
        return result;
    };
    try {
        Bytes executable;
        bool found = false;
        for (const auto &[tag, body] : readDxbcParts(bytecode)) {
            if (tag == 0x45434649)
                return reject("Dynamic linkage is not covered");
            if (tag == 0x52444853 || tag == 0x58454853) {
                if (found)
                    return reject("Ambiguous executable chunks");
                found = true;
                executable = body;
            }
        }
        if (!found)
            return reject("Missing executable code");
        const auto program = readDxbcProgram(executable);
        const auto version = program.header[0] & 0xffff, stage = program.header[0] >> 16;
        if ((version != 0x40 && version != 0x41 && version != 0x50) || stage < 1 || stage > 4)
            return reject("Unsupported geometry shader profile");
        if (terminalDomainCounts && (stage != 4 || version != 0x50))
            return reject("Terminal domain count proof requires a domain shader");
        if (program.instructions.empty())
            return reject("Empty executable code");
        if (terminalGeometryCounts && (terminalDomainCounts || stage != 2))
            return reject("Terminal geometry count proof requires a geometry shader");
        const auto schedule = terminalGeometryCounts ? fixedGeometrySchedule(program) : Json(nullptr);
        const bool fixedGeometry = !schedule.is_null();
        std::set<uint32_t> srvReads, uavReads;
        const auto srvs = shaderSrvDeclarations(bytecode);
        for (uint32_t slot = 0; slot < srvs.size(); ++slot)
            if (srvs[slot])
                srvReads.insert(slot);
        uint32_t token = 2;
        uint64_t budget = 1024 * 1024;
        for (size_t instruction = 0; instruction < program.instructions.size(); ++instruction) {
            const auto &row = program.instructions[instruction];
            result["checked_token"] = token;
            const auto opcode = row[0] & 2047;
            token += uint32_t(row.size());
            // Pinned Microsoft format metadata: these extensions contain only
            // immediate offsets, resource dimensions/strides and return types.
            // They cannot introduce additional resource/register operands.
            std::set<uint32_t> extensions;
            for (size_t word = 1; row.at(word - 1) >> 31; ++word) {
                const auto extension = row.at(word), kind = extension & 63;
                const auto reserved = kind == 1 ? 0x7fe001c0u : kind == 2 ? 0x7f800000u : 0x7fc00000u;
                if (kind < 1 || kind > 3 || !extensions.insert(kind).second || (extension & reserved))
                    return reject("Unsupported or malformed instruction extension");
            }
            if (opcode == 88 || opcode == 161 || opcode == 162)
                continue;
            if (opcode == 4 || opcode == 5 || opcode == 44 || opcode == 53 || opcode == 120 ||
                (opcode >= 144 && opcode <= 146) || opcode == 159 || opcode == 160 || opcode == 190)
                return reject("Unsupported declarations or execution semantics");
            if ((opcode >= 89 && opcode <= 106) || opcode == 143 || (opcode >= 147 && opcode <= 158) ||
                opcode == 206)
                continue;
            if (opcode >= 203)
                return reject("Unsupported execution opcode");
            const auto operands = checkpoint::instructionOperands(row);
            const bool store =
                opcode == 164 || opcode == 166 || opcode == 168 || (opcode >= 169 && opcode <= 177);
            const bool atomic = opcode >= 178 && opcode <= 189;
            if (atomic) {
                const auto [begin, end] = operands.ranges.at(0);
                if (!terminalDomainCounts && !fixedGeometry &&
                    !deadAtomicResult(program, instruction, begin, end, budget))
                    return reject("Atomic result may affect geometry");
            }
            for (size_t i = 0; i < operands.ranges.size(); ++i) {
                const auto [begin, end] = operands.ranges[i];
                const auto kind = (row[begin] >> 12) & 255;
                bool unsupported = false;
                visitOperand(row, begin, [&](uint32_t operand, size_t, bool nested) {
                    const auto type = (operand >> 12) & 255;
                    unsupported |= type == 29 || type == 31 || type >= 38 ||
                                   (nested && (type == 6 || type == 7 || type == 30));
                });
                if (unsupported)
                    return reject("Unsupported resource or operand kind");
                if (kind == 7) {
                    if (end != begin + 2 || (row[begin] & 0xfff00000u) != 0x00100000u ||
                        row[begin + 1] >= 128 || !srvReads.contains(row[begin + 1]))
                        return reject("Nonliteral or undeclared SRV addressing is not covered");
                }
                if (kind != 30)
                    continue;
                if (end != begin + 2 || (row[begin] & 0xfff00000u) != 0x00100000u || row[begin + 1] >= 64)
                    return reject("Nonliteral UAV addressing is not covered");
                if (!((store && i == 0) || (atomic && i == 1))) {
                    uavReads.insert(row[begin + 1]);
                    continue;
                }
                result["write_instructions"].push_back(
                    {{"token", result["checked_token"]},
                     {"opcode", opcode},
                     {"slot", row[begin + 1]},
                     {"result", atomic ? (terminalDomainCounts ? "terminal_domain_value"
                                          : fixedGeometry      ? "terminal_geometry_value"
                                                               : "proved_dead")
                                       : "none"}});
            }
        }
        result.erase("checked_token");
        result["liveness_instructions_checked"] = 1024 * 1024 - budget;
        result["read_dependencies"] = {{"srv_slots", srvReads}, {"uav_slots", uavReads}};
        if (!srvReads.empty() || !uavReads.empty())
            result["scope"] = "immutable_resource_inputs";
        result["proved"] = true;
        result["reason"] = "Atomic results are unobservable; resource reads require runtime alias exclusion";
        if (terminalDomainCounts) {
            result["scope"] = "terminal_domain_counts";
            result["reason"] = "DS values do not change tessellator output counts; all writes require "
                               "upstream alias exclusion";
        }
        if (fixedGeometry) {
            result["terminal_geometry_counts"] = true;
            result["emission_schedule"] = schedule;
            result["scope"] = "terminal_geometry_counts";
            result["reason"] = "GS emission schedule is fixed; all writes require upstream alias exclusion";
        }
        return result;
    } catch (const std::exception &error) {
        result["reason"] = std::string("Unproved bytecode: ") + error.what();
        return result;
    }
}
} // namespace flora
