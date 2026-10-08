#include "Dxbc.h"
namespace flora {
namespace {
// RESINFO returns a view's total mip count in .w independently of resource
// MinLOD. Dimensions in .xyz do not have that guarantee. Accept only checked
// immediate-register forms. SM4.0 sometimes writes all four lanes even for a
// count-only HLSL query; dimension lanes then need a separate non-use proof.
struct MipQuery {
    unsigned slot, temporary, dimensions;
};
std::optional<MipQuery> mipQuery(const std::vector<uint32_t> &row, size_t operands) {
    if ((row[0] & 0x7ff) != 61 || ((row[0] >> 11) & 3) == 3 || (row[0] & 0x007fe000u) ||
        row.size() != operands + 6)
        return {};
    const auto dest = row[operands], mip = row[operands + 2], resource = row[operands + 4];
    if ((dest & ~0xf0u) != 0x00100002u || !(dest & 0xf0) || row[operands + 1] >= 4096)
        return {};
    if (mip != 0x00004001u && !((mip & ~0x30u) == 0x0010000au && row[operands + 3] < 4096))
        return {};
    if ((resource & ~0xff0u) != 0x00107006u || row[operands + 5] >= 128)
        return {};
    unsigned dimensions = 0;
    for (unsigned lane = 0; lane < 4; ++lane)
        if ((dest & (1u << (4 + lane))) && ((resource >> (4 + 2 * lane)) & 3) != 3)
            dimensions |= 1u << lane;
    return MipQuery{row[operands + 5], row[operands + 1], dimensions};
}
struct RegisterUse {
    unsigned type{}, index{}, lanes{};
};
struct FlowOperation {
    RegisterUse destination;
    std::vector<RegisterUse> sources;
    std::vector<size_t> successors;
};
// Deliberately bounded SM4.0 proof. Checked IF, SWITCH and LOOP blocks only;
// no calls, relative
// operands, instruction/operand extensions, memory stores or unfamiliar ALU
// operations are admitted. This is not a general shader optimizer.
std::optional<std::vector<FlowOperation>> boundedSm40(const DxbcProgram &program) {
    if (program.instructions.empty() || program.instructions.size() > 256)
        return {};
    std::vector<FlowOperation> result;
    struct Branch {
        size_t condition;
        std::optional<size_t> otherwise;
        bool selection = false, hasLabel = false, hasDefault = false, loop = false;
        bool caseHasCode = false, caseTerminated = false;
        std::vector<uint32_t> cases;
        std::vector<size_t> breaks;
    };
    std::vector<Branch> branches;
    for (size_t i = 0; i < program.instructions.size(); ++i) {
        const auto &row = program.instructions[i];
        const auto op = row[0] & 0x7ff;
        if (row[0] & 0x80000000u)
            return {};
        FlowOperation instruction;
        instruction.successors = {i + 1};
        if (!branches.empty() && branches.back().selection && op != 6 && op != 10 && op != 23) {
            auto &selection = branches.back();
            // Accept empty shared labels, but require a direct unconditional
            // BREAK after each nonempty case. No executable fallthrough or
            // code before the first label / after a terminating BREAK.
            if (!selection.hasLabel || selection.caseTerminated) return {};
            selection.caseHasCode = true;
        }
        if (op == 62) {
            if (row.size() != 1 || i + 1 != program.instructions.size() || !branches.empty())
                return {};
            instruction.successors.clear();
        } else if (op == 31 || op == 76) {
            // Conditions/selectors use one component. Visit every outcome,
            // without pruning paths from a presumed value or observed image.
            if (row.size() != 3 || (row[0] & (op == 31 ? 0x00fbf800u : 0x00fff800u)) || branches.size() >= 64)
                return {};
            const auto token = row[1], type = (token >> 12) & 255;
            if (op == 76 && token == 0x00004001u) {
                // Still visit every case; literal selector values do not prune
                // a path from this dependency proof.
            } else if ((type != 0 && type != 1) ||
                (token & ~0x30u) != (0x0010000au | (type << 12)) || row[2] >= 4096)
                return {};
            else instruction.sources.push_back({type, row[2], 1u << ((token >> 4) & 3)});
            branches.push_back({i, {}, op == 76});
            if (op == 76) instruction.successors.clear();
        } else if (op == 48 || op == 22) { // LOOP / ENDLOOP.
            if (row.size() != 1 || (row[0] & 0x00fff800u)) return {};
            if (op == 48) {
                if (branches.size() >= 64) return {};
                Branch loop{i};
                loop.loop = true;
                branches.push_back(std::move(loop));
            } else {
                if (branches.empty() || !branches.back().loop) return {};
                const auto &loop = branches.back();
                instruction.successors = {loop.condition + 1};
                for (const auto at : loop.breaks) result[at].successors.push_back(i + 1);
                branches.pop_back();
            }
        } else if (op == 18 || op == 21) {
            if (row.size() != 1 || (row[0] & 0x00fff800u) || branches.empty() ||
                branches.back().selection || branches.back().loop)
                return {};
            auto &branch = branches.back();
            if (op == 18) {
                if (branch.otherwise) return {};
                branch.otherwise = i;
                result[branch.condition].successors.push_back(i + 1);
            } else {
                if (branch.otherwise) result[*branch.otherwise].successors = {i + 1};
                else result[branch.condition].successors.push_back(i + 1);
                branches.pop_back();
            }
        } else if (op == 6 || op == 10 || op == 23) {
            if (branches.empty() || !branches.back().selection || (row[0] & 0x00fff800u) ||
                row.size() != (op == 6 ? 3 : 1)) return {};
            auto &selection = branches.back();
            if (selection.caseHasCode && !selection.caseTerminated) return {};
            if (op == 23) {
                if (!selection.hasLabel) return {};
                if (!selection.hasDefault) result[selection.condition].successors.push_back(i + 1);
                for (const auto at : selection.breaks) result[at].successors.push_back(i + 1);
                branches.pop_back();
            } else {
                if (op == 6) {
                    if (row[1] != 0x00004001u ||
                        std::find(selection.cases.begin(), selection.cases.end(), row[2]) != selection.cases.end())
                        return {};
                    selection.cases.push_back(row[2]);
                } else {
                    if (selection.hasDefault) return {};
                    selection.hasDefault = true;
                }
                selection.hasLabel = true;
                selection.caseHasCode = selection.caseTerminated = false;
                result[selection.condition].successors.push_back(i + 1);
            }
        } else if (op == 2 || op == 3 || op == 7 || op == 8) { // BREAK[C] / CONTINUE[C].
            const bool conditional = op == 3 || op == 8, continuing = op == 7 || op == 8;
            if (row.size() != (conditional ? 3 : 1) ||
                (row[0] & (conditional ? 0x00fbf800u : 0x00fff800u))) return {};
            if (conditional) {
                const auto token = row[1], type = (token >> 12) & 255;
                if ((type != 0 && type != 1) ||
                    (token & ~0x30u) != (0x0010000au | (type << 12)) || row[2] >= 4096)
                    return {};
                instruction.sources.push_back({type, row[2], 1u << ((token >> 4) & 3)});
            } else instruction.successors.clear();
            const auto owner = std::find_if(branches.rbegin(), branches.rend(),
                [continuing](const auto &block) { return block.loop || (!continuing && block.selection); });
            if (owner == branches.rend() || (owner->selection && !owner->hasLabel)) return {};
            if (continuing) instruction.successors.push_back(owner->condition + 1);
            else {
                owner->breaks.push_back(i); // Patched to after the owning ENDLOOP/ENDSWITCH.
                if (!conditional && branches.back().selection) owner->caseTerminated = true;
            }
        } else if (op == 88 || op == 90 || op == 101 || op == 104 || op == 106) {
            // Resource declarations are checked by shaderSrvDeclarations;
            // the other accepted declarations neither read nor write temps.
            const size_t size = op == 88 ? 4 : op == 104 ? 2 : op == 106 ? 1 : 3;
            if (row.size() != size)
                return {};
        } else {
            const unsigned count = (op == 54 || op == 86) ? 1
                                   : (op == 0 || op == 1 || op == 30 || op == 32 || op == 45 || op == 56 || op == 61 || op == 79 || op == 80) ? 2
                                   : op == 69 ? 3 : 0;
            if (!count || row.size() < 3)
                return {};
            const auto dest = row[1];
            const auto type = (dest >> 12) & 255;
            if ((type != 0 && type != 2) || (dest & ~0xf0u) != (0x00100002u | (type << 12)) ||
                !(dest & 0xf0) || row[2] >= 4096)
                return {};
            instruction.destination = {type, row[2], (dest >> 4) & 15};
            size_t pos = 3;
            for (unsigned source = 0; source < count; ++source) {
                if (pos == row.size())
                    return {};
                const auto token = row[pos++];
                if (token == 0x00004001u || token == 0x00004002u) {
                    const size_t words = token == 0x00004001u ? 1 : 4;
                    if (words > row.size() - pos)
                        return {};
                    pos += words; // Literal bit patterns are not operands.
                    continue;
                }
                const auto sourceType = (token >> 12) & 255;
                if (sourceType != 0 && sourceType != 1 && sourceType != 6 && sourceType != 7)
                    return {};
                unsigned lanes = 0;
                if ((token & ~0xff0u) == (0x00100006u | (sourceType << 12))) {
                    for (unsigned lane = 0; lane < 4; ++lane)
                        lanes |= 1u << ((token >> (4 + 2 * lane)) & 3);
                } else if ((token & ~0x30u) == (0x0010000au | (sourceType << 12)))
                    lanes = 1u << ((token >> 4) & 3);
                else if (sourceType != 6 || token != 0x00106000u)
                    return {};
                if (pos == row.size() || row[pos] >= 4096)
                    return {};
                instruction.sources.push_back({sourceType, row[pos++], lanes});
            }
            if (pos != row.size())
                return {};
        }
        result.push_back(std::move(instruction));
    }
    if ((program.instructions.back()[0] & 0x7ff) != 62)
        return {};
    return result;
}
bool dimensionsUnused(const std::vector<FlowOperation> &program, size_t at, MipQuery query) {
    // Monotone union also follows loop backedges. Each node gains at most four
    // lanes, so the worklist has at most 4 * instruction-count insertions.
    // An overwrite on just one incoming path cannot erase another path's lane.
    std::vector<unsigned> incoming(program.size());
    std::vector<size_t> work;
    if (at + 1 < program.size()) {
        incoming[at + 1] = query.dimensions;
        work.push_back(at + 1);
    }
    for (size_t next = 0; next < work.size(); ++next) {
        const auto i = work[next];
        auto pending = incoming[i];
        if (!pending) continue;
        const auto &operation = program[i];
        // Sources are read before a destination overwrites the same register.
        // Source swizzles are conservatively read in full, even for masked ALU.
        for (const auto &source : operation.sources)
            if (source.type == 0 && source.index == query.temporary && (source.lanes & pending))
                return false;
        const auto &dest = operation.destination;
        if (dest.type == 0 && dest.index == query.temporary)
            pending &= ~dest.lanes;
        for (const auto successor : operation.successors)
            if (successor < program.size() && (pending & ~incoming[successor])) {
                incoming[successor] |= pending;
                work.push_back(successor);
            }
    }
    return true; // Any remaining lanes die at the checked final RET.
}
} // namespace
std::array<bool, 128> shaderSrvLodDependencies(Bytes bytes) {
    auto required = shaderSrvDeclarations(bytes);
    Bytes code;
    for (const auto &[tag, body] : readDxbcParts(bytes)) {
        if (tag == 0x45434649)
            return required;
        if (tag == 0x52444853 || tag == 0x58454853)
            code = body;
    }
    if (code.empty())
        return required;
    const auto program = readDxbcProgram(code);
    const auto version = program.header[0] & 0xffff;
    if ((version != 0x40 && version != 0x41 && version != 0x50) || (program.header[0] >> 16) > 5)
        return required;
    std::array<bool, 128> metadata{}, other{};
    const auto flow = version == 0x40 ? boundedSm40(program) : std::nullopt;
    for (size_t instruction = 0; instruction < program.instructions.size(); ++instruction) {
        const auto &row = program.instructions[instruction];
        const auto op = row[0] & 0x7ff;
        if (op == 53 || op >= 206 || op == 107 || op == 112 || op == 120 || op == 144 || op == 145 ||
            op == 146)
            return required; // Custom/interface/reserved or unfamiliar instruction families.
        if (op == 88 || op == 161 || op == 162)
            continue; // Declaration forms were checked by shaderSrvDeclarations.
        size_t operands = 1;
        auto extended = row[0] & 0x80000000u;
        while (extended) {
            if (operands >= row.size())
                return required;
            const auto token = row[operands++], kind = token & 63;
            if (kind < 1 || kind > 3 || (op == 61 && kind == 1))
                return required;
            extended = token & 0x80000000u;
        }
        if (const auto query = mipQuery(row, operands)) {
            if (!query->dimensions || (flow && dimensionsUnused(*flow, instruction, *query))) {
                if (!required[query->slot])
                    return required;
                metadata[query->slot] = true;
                continue;
            }
        }
        // Every direct SM4/5 SRV reference carries a RESOURCE operand token.
        // Scan conservatively: a literal that resembles one can retain a
        // dependency, never remove one. Relative/extended forms retain all.
        for (size_t i = operands; i < row.size(); ++i) {
            const auto word = row[i];
            if (((word >> 12) & 255) != 7)
                continue;
            if ((word & 0xfffff000u) != 0x00107000u || i + 1 == row.size() || row[i + 1] >= required.size() ||
                !required[row[i + 1]])
                return required;
            other[row[i + 1]] = true;
        }
    }
    for (size_t i = 0; i < required.size(); ++i)
        if (metadata[i] && !other[i])
            required[i] = false;
    return required;
}
} // namespace flora
