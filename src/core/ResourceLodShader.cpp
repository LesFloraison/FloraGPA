#include "Dxbc.h"
namespace flora {
namespace {
// RESINFO returns a view's total mip count in .w independently of resource
// MinLOD. Dimensions in .xyz do not have that guarantee. Accept only checked
// immediate-register forms and only lanes selecting .w.
std::optional<unsigned> mipCountOnly(const std::vector<uint32_t> &row, size_t operands) {
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
    for (unsigned lane = 0; lane < 4; ++lane)
        if ((dest & (1u << (4 + lane))) && ((resource >> (4 + 2 * lane)) & 3) != 3)
            return {};
    return row[operands + 5];
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
    for (const auto &row : program.instructions) {
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
        if (const auto slot = mipCountOnly(row, operands)) {
            if (!required[*slot])
                return required;
            metadata[*slot] = true;
            continue;
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
