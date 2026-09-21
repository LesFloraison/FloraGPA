#include "DxbcSystemId.h"
#include "DxbcInspection.h"
#include "DxbcOperandArities.h"
#include <algorithm>
#include <bit>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
using Row = std::vector<uint32_t>;
bool declaration(uint32_t op) { return op == 53 || (op >= 88 && op <= 106) || (op >= 143 && op <= 162); }
size_t rewrite(Row &row, size_t pos, uint32_t input, uint32_t temporary, size_t &seen, unsigned depth = 0) {
    if (depth > 32 || pos >= row.size())
        throw std::runtime_error("Invalid system-ID operand bounds");
    const auto start = pos;
    const auto token = row[pos++], kind = (token >> 12) & 255, dimensions = (token >> 20) & 3;
    for (auto extended = token >> 31; extended;)
        extended = row.at(pos++) >> 31;
    if (kind == 1) {
        if (dimensions != 1 || ((token >> 22) & 7))
            throw std::runtime_error("Dynamic VS input indexing cannot be remapped");
        if (row.at(pos) == input) {
            row[start] = token & ~(255u << 12);
            row[pos] = temporary;
            ++seen;
        }
    }
    for (unsigned d = 0; d < dimensions; ++d) {
        const auto representation = (token >> (22 + d * 3)) & 7;
        if (representation == 0 || representation == 3)
            ++pos;
        else if (representation == 1 || representation == 4)
            pos += 2;
        else if (representation != 2)
            throw std::runtime_error("Unsupported system-ID index representation");
        if (representation >= 2 && representation <= 4)
            pos = rewrite(row, pos, input, temporary, seen, depth + 1);
    }
    if (kind == 4 || kind == 5) {
        const auto components = token & 3;
        if ((components != 1 && components != 2) || dimensions || (kind == 5 && components != 2))
            throw std::runtime_error("Unsupported system-ID immediate layout");
        pos += components == 1 ? 1 : 4;
    }
    if (pos > row.size())
        throw std::runtime_error("Truncated system-ID operand");
    return pos;
}
} // namespace
SystemIdShader offsetSystemId(Bytes original, uint32_t offset, uint32_t systemValue) {
    dxbc_detail::Parts parts;
    std::set<uint32_t> tags;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate DXBC chunk");
        parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    const auto key = tags.contains(0x58454853u) ? 0x58454853u : 0x52444853u;
    auto code = readDxbcProgram(dxbc_detail::part(parts, key));
    if (code.header[0] != 0x10040 && code.header[0] != 0x10041 && code.header[0] != 0x10050)
        throw std::runtime_error("System-ID offset requires an SM4/SM5 vertex shader");
    if (offset && (!tags.contains(0x4e475349) || tags.contains(0x31475349) || tags.contains(0x35475349)))
        throw std::runtime_error("System-ID offset requires a supported ISGN input signature");
    if (systemValue != 6 && systemValue != 8)
        throw std::runtime_error("Only VertexID and InstanceID offsets are supported");
    const auto inputs = tags.contains(0x4e475349)
                            ? dxbc_detail::signature(dxbc_detail::part(parts, 0x4e475349))
                            : Json::array();
    Json ids = Json::array();
    for (const auto &input : inputs)
        if (input.at("system_value") == systemValue)
            ids.push_back(input);
    if (!offset || ids.empty())
        return {{original.begin(), original.end()}, {{"changed", false}, {"offset", offset}}};
    const auto mask = ids[0].at("mask").get<uint32_t>();
    if (ids.size() != 1 || ids[0].at("component_type") != 1 || !std::has_single_bit(mask) || mask > 8)
        throw std::runtime_error("Ambiguous or invalid system-ID signature");
    const auto reg = ids[0].at("register").get<uint32_t>();
    uint32_t copied = 0;
    for (const auto &input : inputs)
        if (input.at("register") == reg)
            copied |= input.at("mask").get<uint32_t>();
    Row *temps = nullptr;
    for (auto &row : code.instructions)
        if ((row[0] & 2047) == 104) {
            if (temps || row.size() != 2)
                throw std::runtime_error("Invalid temporary declaration");
            temps = &row;
        }
    const uint32_t temporary = temps ? (*temps)[1] : 0;
    if (temporary >= 4096)
        throw std::runtime_error("System-ID offset needs a free temporary register");
    if (temps)
        ++(*temps)[1];
    size_t seen = 0;
    for (auto &row : code.instructions) {
        const auto op = row[0] & 2047;
        if (declaration(op))
            continue;
        const auto arity = dxbc_detail::operandArities.find(op);
        if (arity == dxbc_detail::operandArities.end())
            throw std::runtime_error("Unrecognized VS instruction " + std::to_string(op));
        size_t pos = 1;
        for (auto extended = row[0] >> 31; extended;)
            extended = row.at(pos++) >> 31;
        if (op == 120)
            ++pos; // Interface-call function index precedes its operand.
        for (unsigned i = 0; i < arity->second; ++i)
            pos = rewrite(row, pos, reg, temporary, seen);
        if (pos != row.size())
            throw std::runtime_error("Unexpected system-ID instruction tail");
    }
    std::vector<Row> added;
    if (!temps)
        added.push_back({0x02000068, 1});
    added.push_back({0x05000036, 0x00100002 | (copied << 4), temporary, 0x00101e46, reg});
    added.push_back({0x0700001e, 0x00100002 | (mask << 4), temporary,
                     0x0010000au | (uint32_t(std::countr_zero(mask)) << 4), temporary, 0x00004001, offset});
    const auto split = std::find_if_not(code.instructions.begin(), code.instructions.end(),
                                        [](const auto &row) { return declaration(row[0] & 2047); });
    code.instructions.insert(split, added.begin(), added.end());
    dxbc_detail::part(parts, key) = writeDxbcProgram(code);
    DxbcParts output;
    for (const auto &[tag, bytes] : parts)
        if (tag != 0x54415453 && tag != 0x47424453 && tag != 0x42445053 && tag != 0x42444c49 &&
            tag != 0x49435253 && tag != 0x4e444c49)
            output.emplace_back(tag, bytes);
    return {makeDxbc(output),
            {{"changed", true},
             {"offset", offset},
             {"input_register", reg},
             {"input_mask", mask},
             {"copied_mask", copied},
             {"temporary", temporary},
             {"rewritten_operands", seen}}};
}
} // namespace flora
