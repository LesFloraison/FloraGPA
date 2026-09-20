#include "DxbcIdentity.h"
#include <QString>
#include <algorithm>
#include <bit>
#include <set>
namespace flora {
namespace {
using Json = nlohmann::json;
constexpr uint32_t ISGN = 0x4e475349, OSGN = 0x4e47534f, SHEX = 0x58454853, SHDR = 0x52444853;
using Parts = std::vector<std::pair<uint32_t, std::vector<uint8_t>>>;
auto findPart(Parts &parts, uint32_t tag) {
    return std::find_if(parts.begin(), parts.end(), [tag](const auto &p) { return p.first == tag; });
}
std::vector<uint8_t> &part(Parts &parts, uint32_t tag) {
    auto found = findPart(parts, tag);
    if (found == parts.end())
        throw std::runtime_error("Missing legacy DXBC signature or program");
    return found->second;
}
void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("DXBC patch bounds");
    std::memcpy(bytes.data() + offset, &value, 4);
}
uint32_t word(Bytes bytes, size_t offset) {
    if (offset > bytes.size())
        throw std::runtime_error("DXBC field bounds");
    return Reader(bytes.subspan(offset)).read<uint32_t>();
}
Json signature(Bytes bytes) {
    Reader reader(bytes);
    auto count = reader.read<uint32_t>();
    reader.skip(4);
    if (count > 256 || uint64_t(count) * 24 > reader.remaining())
        throw std::runtime_error("Legacy signature bounds");
    Json result = Json::array();
    for (uint32_t i = 0; i < count; ++i) {
        auto offset = reader.read<uint32_t>(), index = reader.read<uint32_t>(),
             system = reader.read<uint32_t>(), type = reader.read<uint32_t>(), reg = reader.read<uint32_t>();
        auto mask = reader.read<uint8_t>(), used = reader.read<uint8_t>();
        reader.skip(2);
        if (offset >= bytes.size())
            throw std::runtime_error("Legacy semantic bounds");
        auto end = std::find(bytes.begin() + offset, bytes.end(), uint8_t(0));
        if (end == bytes.end())
            throw std::runtime_error("Unterminated legacy semantic");
        result.push_back(
            {{"semantic", QString::fromUtf8(reinterpret_cast<const char *>(bytes.data() + offset),
                                            end - bytes.begin() - offset)
                              .toStdString()},
             {"index", index},
             {"system_value", system},
             {"component_type", type},
             {"register", reg},
             {"mask", mask},
             {"used_mask", used}});
    }
    return result;
}
using Masks = std::array<uint32_t, 32>;
Masks occupied(const Json &signature, const DxbcProgram &program, unsigned kind) {
    Masks masks{};
    for (const auto &item : signature) {
        const auto reg = item.at("register").get<unsigned>(), mask = item.at("mask").get<unsigned>();
        if (reg >= 32 || !mask || mask > 15)
            throw std::runtime_error("Invalid identity carrier signature register");
        masks[reg] |= mask;
    }
    for (const auto &row : program.instructions) {
        auto opcode = row[0] & 2047;
        if (opcode != 91 && opcode != 95 && opcode != 96 && opcode != 97 && opcode != 101 && opcode != 102 &&
            opcode != 103)
            continue;
        if (row.size() < 2)
            throw std::runtime_error("Truncated identity declaration");
        const auto token = row[1], dimensions = (token >> 20) & 3;
        if (((token >> 12) & 255) != kind)
            continue;
        if ((token >> 31) || (dimensions != 1 && dimensions != 2))
            throw std::runtime_error("Unsupported identity declaration addressing");
        for (unsigned i = 0; i < dimensions; ++i)
            if ((token >> (22 + 3 * i)) & 7)
                throw std::runtime_error("Unsupported identity declaration addressing");
        const auto reg = row.at(1 + dimensions), mask = (token >> 4) & 15,
                   count = opcode == 91 ? row.at(2 + dimensions) : 1;
        if (reg >= 32 || !count || count > 32 - reg || !mask)
            throw std::runtime_error("Identity declaration range");
        for (unsigned i = reg; i < reg + count; ++i)
            masks[i] |= mask;
    }
    return masks;
}
std::pair<unsigned, unsigned> allocate(const Masks &masks, bool whole = false) {
    for (unsigned r = 0; r < 32; ++r)
        if (!masks[r])
            return {r, 0};
    if (!whole)
        for (unsigned r = 0; r < 32; ++r)
            for (unsigned c = 0; c < 4; ++c)
                if (!(masks[r] & (1u << c)))
                    return {r, c};
    throw std::runtime_error(whole ? "VS identity needs a free system input register"
                                   : "VS identity needs two free output components");
}
void appendSignature(Parts &parts, uint32_t tag, const std::string &name, unsigned reg, unsigned mask,
                     unsigned system = 0, bool output = false) {
    for (auto variant : {0x31475349u, 0x35475349u, 0x3147534fu, 0x3547534fu})
        if (findPart(parts, variant) != parts.end())
            throw std::runtime_error("VS identity requires legacy SM4/SM5 signatures");
    auto &payload = part(parts, tag);
    signature(payload); // Validate all existing rows before adjusting their offsets.
    const auto count = word(payload, 0), base = 8 + 24 * count;
    std::vector<std::vector<uint8_t>> rows;
    for (unsigned i = 0; i < count; ++i) {
        std::vector<uint8_t> row(payload.begin() + 8 + 24 * i, payload.begin() + 32 + 24 * i);
        put(row, 0, word(row, 0) + 24);
        rows.push_back(std::move(row));
    }
    std::vector<uint8_t> row(24);
    put(row, 0, uint32_t(payload.size() + 24));
    put(row, 8, system);
    put(row, 12, 1);
    put(row, 16, reg);
    row[20] = uint8_t(mask);
    row[21] = uint8_t(output ? 15 ^ mask : mask);
    rows.push_back(std::move(row));
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return std::pair{word(a, 16), a[20]} < std::pair{word(b, 16), b[20]};
    });
    std::vector<uint8_t> result(payload.begin(), payload.begin() + 8);
    put(result, 0, count + 1);
    for (const auto &entry : rows)
        result.insert(result.end(), entry.begin(), entry.end());
    result.insert(result.end(), payload.begin() + base, payload.end());
    result.insert(result.end(), name.begin(), name.end());
    result.resize(result.size() + (4 - name.size() % 4), 0);
    payload = std::move(result);
}
// Walk operands, including nested relative indices and immediate64 vectors, so
// literals that resemble UAV registers never count as resource use.
size_t operand(const std::vector<uint32_t> &row, size_t pos, std::set<unsigned> &found, unsigned depth = 0) {
    if (depth > 32 || pos >= row.size())
        throw std::runtime_error("DXBC operand bounds");
    auto token = row[pos++], kind = (token >> 12) & 255, dimensions = (token >> 20) & 3;
    for (auto extended = token >> 31; extended;)
        extended = row.at(pos++) >> 31;
    if (kind == 30) {
        if (dimensions != 1 || ((token >> 22) & 7))
            throw std::runtime_error("UAV inspection requires static SM5 registers");
        found.insert(row.at(pos));
    }
    for (unsigned i = 0; i < dimensions; ++i) {
        auto repr = (token >> (22 + i * 3)) & 7;
        if (repr == 0 || repr == 3)
            ++pos;
        else if (repr == 1 || repr == 4)
            pos += 2;
        else if (repr != 2)
            throw std::runtime_error("Unsupported DXBC index representation");
        if (repr >= 2 && repr <= 4)
            pos = operand(row, pos, found, depth + 1);
    }
    if (kind == 4 || kind == 5) {
        const auto components = token & 3;
        if ((components != 1 && components != 2) || dimensions || (kind == 5 && components != 2))
            throw std::runtime_error("Unsupported DXBC immediate layout");
        pos += components == 1 ? 1 : 4;
    }
    if (pos > row.size())
        throw std::runtime_error("Truncated DXBC operand");
    return pos;
}
bool hasUav(const DxbcProgram &program, Parts &parts) {
    std::set<unsigned> declared, used;
    const std::map<unsigned, unsigned> counts{
        {61, 3},  {121, 2}, {156, 1}, {157, 1}, {158, 1}, {163, 3}, {164, 3}, {165, 3},
        {166, 3}, {167, 4}, {168, 4}, {169, 3}, {170, 3}, {171, 3}, {172, 4}, {173, 3},
        {174, 3}, {175, 3}, {176, 3}, {177, 3}, {178, 2}, {179, 2}, {180, 4}, {181, 4},
        {182, 4}, {183, 4}, {184, 4}, {185, 5}, {186, 4}, {187, 4}, {188, 4}, {189, 4}};
    for (const auto &row : program.instructions) {
        const auto op = row[0] & 2047;
        if (op >= 225 && op <= 227)
            throw std::runtime_error("UAV feedback inspection is not implemented");
        auto count = counts.find(op);
        if (count == counts.end())
            continue;
        size_t pos = 1;
        for (auto extended = row[0] >> 31; extended;)
            extended = row.at(pos++) >> 31;
        std::set<unsigned> found;
        for (unsigned i = 0; i < count->second; ++i)
            pos = operand(row, pos, found);
        if (pos + (op == 156 || op == 158 ? 1 : 0) != row.size())
            throw std::runtime_error("Unexpected UAV instruction layout");
        used.insert(found.begin(), found.end());
        if (op >= 156 && op <= 158)
            declared.insert(found.begin(), found.end());
    }
    for (auto slot : used)
        if (!declared.contains(slot))
            throw std::runtime_error("UAV use lacks a declaration");
    for (auto slot : declared)
        if (slot >= 64)
            throw std::runtime_error("Declared UAV exceeds device limit");
    auto rdef = findPart(parts, 0x46454452);
    if (rdef != parts.end()) {
        const auto &bytes = rdef->second;
        auto count = word(bytes, 8), offset = word(bytes, 12);
        if (offset > bytes.size() || uint64_t(count) * 32 > bytes.size() - offset)
            throw std::runtime_error("Reflected UAV bounds");
        for (unsigned i = 0; i < count; ++i) {
            auto type = word(bytes, offset + i * 32 + 4);
            if (type != 4 && type != 6 && (type < 8 || type > 11))
                continue;
            auto slot = word(bytes, offset + i * 32 + 20), size = word(bytes, offset + i * 32 + 24);
            if (!size || slot >= 64 || size > 64 - slot)
                throw std::runtime_error("Invalid reflected UAV range");
        }
    }
    return !declared.empty();
}
std::vector<uint32_t> instruction(unsigned opcode, unsigned reg, unsigned mask, unsigned kind,
                                  std::vector<uint32_t> tail = {}) {
    std::vector<uint32_t> row{opcode, 0x100002 | (kind << 12) | (mask << 4), reg};
    row.insert(row.end(), tail.begin(), tail.end());
    row[0] |= uint32_t(row.size()) << 24;
    return row;
}
} // namespace
VertexIdentityShader instrumentVertexIdentity(Bytes original, uint32_t instances) {
    Parts parts;
    std::set<uint32_t> tags;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate DXBC chunk");
        parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    const auto key = tags.contains(SHEX) ? SHEX : SHDR;
    auto program = readDxbcProgram(part(parts, key));
    if (program.header[0] != 0x10040 && program.header[0] != 0x10041 && program.header[0] != 0x10050)
        throw std::runtime_error("VS identities require SM4/SM5 legacy vertex shaders");
    const auto vin = signature(part(parts, ISGN)), vout = signature(part(parts, OSGN));
    auto inputs = occupied(vin, program, 1), outputs = occupied(vout, program, 2);
    std::set<std::string> names;
    for (const auto &entry : vout)
        names.insert(QString::fromStdString(entry.at("semantic")).toUpper().toStdString());
    const bool writable = program.header[0] == 0x10050 && hasUav(program, parts);
    std::vector<std::vector<uint32_t>> declarations, moves;
    Json markers = Json::object();
    for (const auto &[kind, system, semantic] :
         {std::tuple{"vertex", 6u, "SV_VertexID"}, std::tuple{"instance", 8u, "SV_InstanceID"}}) {
        const bool constant = system == 8 && instances <= 1;
        bool consumed = false;
        Json inputRegister = nullptr, inputComponent = nullptr;
        std::vector<uint32_t> source{0x4001, 0};
        if (!constant) {
            Json existing = Json::array();
            for (const auto &item : vin)
                if (item.at("system_value") == system)
                    existing.push_back(item);
            unsigned reg = 0, component = 0;
            if (!existing.empty()) {
                const auto &item = existing[0];
                auto mask = item.at("mask").get<unsigned>();
                if (existing.size() != 1 || item.at("component_type") != 1 || !std::has_single_bit(mask) ||
                    mask > 8)
                    throw std::runtime_error("Invalid native VS identity signature");
                reg = item.at("register").get<unsigned>();
                component = std::countr_zero(mask);
                bool declared = false;
                for (const auto &row : program.instructions)
                    declared |= (row[0] & 2047) == 96 && row.size() == 4 && row[2] == reg &&
                                row[3] == system && (((row[1] >> 4) & 15) & mask);
                consumed = declared && (item.at("used_mask").get<unsigned>() & mask);
                if (!declared)
                    declarations.push_back(instruction(96, reg, mask, 1, {system}));
                auto &payload = part(parts, ISGN);
                for (unsigned i = 0; i < word(payload, 0); ++i)
                    if (word(payload, 8 + 24 * i + 8) == system)
                        payload[8 + 24 * i + 21] |= uint8_t(mask);
            } else {
                std::tie(reg, component) = allocate(inputs, true);
                inputs[reg] = 1;
                appendSignature(parts, ISGN, semantic, reg, 1, system);
                declarations.push_back(instruction(96, reg, 1, 1, {system}));
            }
            if (writable && !consumed)
                throw std::runtime_error("VS identity cannot add system inputs to a VS with UAV access: "
                                         "native invocation reuse may change");
            inputRegister = reg;
            inputComponent = component;
            source = {0x10100a | (component << 4), reg};
        }
        const auto [reg, component] = allocate(outputs);
        const auto strategy = outputs[reg] ? "component" : "register";
        const auto mask = 1u << component;
        outputs[reg] |= mask;
        std::string name = std::string("FLORA_") + (system == 6 ? "VERTEX_ID" : "INSTANCE_ID");
        while (names.contains(name))
            name += '_';
        names.insert(name);
        appendSignature(parts, OSGN, name, reg, mask, 0, true);
        declarations.push_back(instruction(101, reg, mask, 2));
        moves.push_back(instruction(54, reg, mask, 2, source));
        markers[kind] = {{"semantic", name},
                         {"register", reg},
                         {"component", component},
                         {"allocation", strategy},
                         {"input_register", inputRegister},
                         {"input_component", inputComponent},
                         {"original_input_consumed", consumed},
                         {"constant_zero", constant}};
    }
    auto split = program.instructions.begin();
    while (split != program.instructions.end()) {
        const auto op = (*split)[0] & 2047;
        if (!(op == 53 || (op >= 88 && op <= 106) || (op >= 143 && op <= 162)))
            break;
        ++split;
    }
    declarations.insert(declarations.end(), moves.begin(), moves.end());
    program.instructions.insert(split, declarations.begin(), declarations.end());
    part(parts, key) = writeDxbcProgram(program);
    const std::set<uint32_t> stale{0x54415453, 0x47424453, 0x42445053, 0x42444c49, 0x49435253, 0x4e444c49};
    DxbcParts assembled;
    for (const auto &[tag, bytes] : parts)
        if (!stale.contains(tag))
            assembled.emplace_back(tag, bytes);
    return {makeDxbc(assembled), std::move(markers)};
}
} // namespace flora
