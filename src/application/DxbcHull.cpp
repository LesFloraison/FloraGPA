#include "DxbcHull.h"
#include "DxbcInspection.h"
#include "DxbcOperandArities.h"
#include <QString>
#include <algorithm>
#include <bit>
namespace flora {
namespace {
using Json = nlohmann::json;
using Row = std::vector<uint32_t>;
using Rows = std::vector<Row>;
constexpr uint32_t SHEX = 0x58454853, SHDR = 0x52444853, ISGN = 0x4e475349, OSGN = 0x4e47534f,
                   PCSG = 0x47534350;
Row op(uint32_t code, std::initializer_list<Row> args = {}) {
    Row result{code};
    for (const auto &a : args)
        result.insert(result.end(), a.begin(), a.end());
    if (result.size() > 127)
        throw std::runtime_error("HS instruction length");
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Row dst(unsigned r, unsigned mask = 15, unsigned kind = 0) {
    return {0x100002 | (kind << 12) | (mask << 4), r};
}
Row src(unsigned r, unsigned c) { return {0x10000a | (c << 4), r}; }
Row imm(uint32_t v) { return {0x4001, v}; }
unsigned swizzle(unsigned mask) {
    unsigned value = 0, first = std::countr_zero(mask);
    for (unsigned c = 0; c < 4; ++c)
        value |= (mask & (1u << c) ? c : first) << (2 * c);
    return value;
}
bool declaration(unsigned c) { return c == 53 || (c >= 88 && c <= 106) || (c >= 143 && c <= 162); }
bool phase(unsigned c) { return c >= 114 && c <= 116; }
struct Program {
    dxbc_detail::Parts parts;
    DxbcProgram program;
    uint32_t key = SHDR;
    explicit Program(Bytes bytes) {
        std::set<uint32_t> tags;
        for (auto &[tag, data] : readDxbcParts(bytes)) {
            if (!tags.insert(tag).second)
                throw std::runtime_error("Duplicate HS DXBC chunk");
            parts.emplace_back(tag, std::vector<uint8_t>(data.begin(), data.end()));
        }
        if (tags.contains(SHEX))
            key = SHEX;
        program = readDxbcProgram(dxbc_detail::part(parts, key));
    }
    Json signature(uint32_t tag) { return dxbc_detail::signature(dxbc_detail::part(parts, tag)); }
    std::vector<uint8_t> finish() {
        dxbc_detail::part(parts, key) = writeDxbcProgram(program);
        std::erase_if(parts, [](const auto &p) {
            return std::set<uint32_t>{0x54415453, 0x47424453, 0x42445053, 0x42444c49, 0x49435253, 0x4e444c49}
                .contains(p.first);
        });
        DxbcParts refs;
        for (auto &[tag, bytes] : parts)
            refs.emplace_back(tag, bytes);
        return makeDxbc(refs);
    }
};
Json schema(Program &p) {
    if (p.program.header[0] != 0x30050)
        throw std::runtime_error("Hull output requires an SM5 hull shader");
    auto cp = p.signature(OSGN), pc = p.signature(PCSG);
    if (cp.empty() || pc.empty())
        throw std::runtime_error("HS needs control-point and patch-constant signatures");
    auto registers = [](const Json &fields) {
        unsigned n = 0;
        for (const auto &f : fields) {
            const auto r = f.at("register").get<unsigned>(), m = f.at("mask").get<unsigned>(),
                       t = f.at("component_type").get<unsigned>();
            if (r >= 32 || !m || m > 15 || t < 1 || t > 3)
                throw std::runtime_error("Unsupported HS signature");
            n = std::max(n, r + 1);
        }
        return n;
    };
    auto declared = [&](unsigned code) {
        unsigned n = 0, v = 0;
        for (auto &row : p.program.instructions)
            if ((row[0] & 2047) == code) {
                ++n;
                v = (row[0] >> 11) & 63;
            }
        if (n != 1)
            throw std::runtime_error("Missing or ambiguous HS declaration");
        return v;
    };
    const auto inputs = declared(147), outputs = declared(148), domain = declared(149), cr = registers(cp),
               pr = registers(pc);
    if (!inputs || inputs > 32 || !outputs || outputs > 32)
        throw std::runtime_error("HS control point count");
    return {{"input_control_points", inputs},
            {"output_control_points", outputs},
            {"domain", domain == 1   ? Json("isoline")
                       : domain == 2 ? Json("tri")
                       : domain == 3 ? Json("quad")
                                     : Json(nullptr)},
            {"control_point_registers", cr},
            {"patch_constant_registers", pr},
            {"control_point_stride", cr * 16},
            {"patch_constant_offset", outputs * cr * 16},
            {"patch_stride", (outputs * cr + pr) * 16},
            {"control_point_signature", cp},
            {"patch_constant_signature", pc}};
}
std::tuple<unsigned, unsigned, std::string> allocate(const dxbc_detail::Masks &m, bool whole = false) {
    for (unsigned r = 0; r < 32; ++r)
        if (!m[r])
            return {r, 0, "register"};
    if (!whole)
        for (unsigned r = 0; r < 32; ++r)
            for (unsigned c = 0; c < 4; ++c)
                if (!(m[r] & (1u << c)))
                    return {r, c, "component"};
    throw std::runtime_error(whole ? "HS identity needs a free VS system input register"
                                   : "HS identity needs a free VS output / HS input component");
}
} // namespace
Json hullOutputSchema(Bytes original) {
    Program p(original);
    return schema(p);
}
HullInstanceShaders carryHullInstance(Bytes vertex, Bytes hull) {
    Program v(vertex), h(hull);
    const auto version = v.program.header[0];
    if ((version != 0x10040 && version != 0x10041 && version != 0x10050) || h.program.header[0] != 0x30050)
        throw std::runtime_error("HS identity requires SM4/SM5 VS and SM5 HS");
    auto vo = v.signature(OSGN), hi = h.signature(ISGN), vi = v.signature(ISGN);
    auto masks = dxbc_detail::occupied(vo, v.program, 2), hin = dxbc_detail::occupied(hi, h.program, 1),
         hcp = dxbc_detail::occupied(hi, h.program, 25);
    for (unsigned r = 0; r < 32; ++r)
        masks[r] |= hin[r] | hcp[r];
    auto [reg, component, allocation] = allocate(masks);
    std::set<std::string> names;
    for (const auto *fields : {&vo, &hi})
        for (const auto &f : *fields)
            names.insert(QString::fromStdString(f.at("semantic")).toUpper().toStdString());
    std::string name = "FLORA_INSTANCE_ID";
    while (names.contains(name))
        name += '_';
    Json existing = Json::array();
    for (auto &f : vi)
        if (f.at("system_value") == 8)
            existing.push_back(f);
    Rows added;
    bool consumed = false;
    unsigned input = 0, ic = 0;
    std::string inputAllocation = "existing";
    if (!existing.empty()) {
        const auto &f = existing[0];
        const auto mask = f.at("mask").get<unsigned>();
        if (existing.size() != 1 || f.at("component_type") != 1 || !std::has_single_bit(mask) || mask > 8)
            throw std::runtime_error("Invalid native VS InstanceID signature");
        input = f.at("register");
        ic = std::countr_zero(mask);
        bool declared = false;
        for (const auto &row : v.program.instructions)
            declared |= (row[0] & 2047) == 96 && row.size() == 4 && row[2] == input && row[3] == 8 &&
                        (((row[1] >> 4) & 15) & mask);
        consumed = declared && (f.at("used_mask").get<unsigned>() & mask);
        if (!declared)
            added.push_back(op(96, {dst(input, mask, 1), {8}}));
        auto &payload = dxbc_detail::part(v.parts, ISGN);
        auto count = Reader(payload).read<uint32_t>();
        for (unsigned i = 0; i < count; ++i)
            if (Reader(Bytes(payload).subspan(8 + i * 24 + 8)).read<uint32_t>() == 8)
                payload.at(8 + i * 24 + 21) |= uint8_t(mask);
    } else {
        std::tie(input, ic, inputAllocation) = allocate(dxbc_detail::occupied(vi, v.program, 1), true);
        dxbc_detail::appendSignature(v.parts, ISGN, "SV_InstanceID", input, 1u << ic, 8, false);
        added.push_back(op(96, {dst(input, 1u << ic, 1), {8}}));
    }
    if (!consumed && version == 0x10050 && !dxbc_detail::uavSlots(v.program, v.parts).empty())
        throw std::runtime_error(
            "Adding InstanceID to a VS with UAV access changes native reuse and side effects");
    dxbc_detail::appendSignature(v.parts, OSGN, name, reg, 1u << component, 0, true);
    dxbc_detail::appendSignature(h.parts, ISGN, name, reg, 1u << component, 0, false);
    added.push_back(op(101, {dst(reg, 1u << component, 2)}));
    added.push_back(op(54, {dst(reg, 1u << component, 2), {0x10100a | (ic << 4), input}}));
    auto split = std::find_if(v.program.instructions.begin(), v.program.instructions.end(),
                              [](const Row &row) { return !declaration(row[0] & 2047); });
    v.program.instructions.insert(split, added.begin(), added.end());
    Json identity{{"register", reg},
                  {"component", component},
                  {"allocation", allocation},
                  {"semantic", name},
                  {"vs_input_register", input},
                  {"vs_input_component", ic},
                  {"vs_input_allocation", inputAllocation},
                  {"existing_instance_input", !existing.empty()},
                  {"original_instance_input_consumed", consumed},
                  {"system_ids_rewritten", false}};
    return {v.finish(), h.finish(), identity};
}
OutputLogShader instrumentHullOutputs(Bytes original, uint32_t slot, uint32_t patches, const Json &identity,
                                      uint32_t perInstance) {
    if (slot >= 64)
        throw std::runtime_error("HS capture slot");
    Program p(original);
    auto m = schema(p);
    const auto cp = m.at("input_control_points").get<unsigned>(),
               outputs = m.at("output_control_points").get<unsigned>(),
               stride = m.at("patch_stride").get<unsigned>();
    const uint64_t size = uint64_t(patches) * stride;
    if (size * 2 > 256ull * 1024 * 1024)
        throw std::runtime_error("HS attribute and validity storage exceeds 256 MiB");
    if (!identity.is_null() &&
        (identity.at("register").get<unsigned>() >= 32 || identity.at("component").get<unsigned>() >= 4 ||
         uint64_t(perInstance) * stride > UINT32_MAX))
        throw std::runtime_error("Invalid HS instance addressing");
    Rows globals;
    std::vector<Rows> phases;
    for (auto &row : p.program.instructions) {
        if (phase(row[0] & 2047))
            phases.emplace_back();
        if (phases.empty())
            globals.push_back(row);
        else
            phases.back().push_back(row);
    }
    if (phases.empty())
        throw std::runtime_error("Hull shader has no executable phases");
    for (auto &row : globals)
        if ((row[0] & 2047) >= 156 && (row[0] & 2047) <= 158 && row.at(2) == slot)
            throw std::runtime_error("Private HS UAV slot is occupied");
    globals.push_back(op(157, {{0x11e000, slot}}));
    const bool synthesized = std::none_of(phases.begin(), phases.end(),
                                          [](const Rows &rows) { return (rows[0][0] & 2047) == 114; });
    if (synthesized) {
        if (cp != outputs)
            throw std::runtime_error("Implicit HS control point counts differ");
        dxbc_detail::Masks masks{}, inputs{};
        for (auto &f : m.at("control_point_signature"))
            masks.at(f.at("register")) |= f.at("mask").get<unsigned>();
        for (auto &f : p.signature(ISGN))
            inputs.at(f.at("register")) |= f.at("mask").get<unsigned>();
        Rows rows{op(114), op(95, {{0x16000}})};
        for (unsigned r = 0; r < 32; ++r)
            if (masks[r]) {
                if (masks[r] & ~inputs[r])
                    throw std::runtime_error("Implicit HS output lacks input components");
                rows.push_back(op(95, {{0x201002 | (masks[r] << 4), cp, r}}));
            }
        for (unsigned r = 0; r < 32; ++r)
            if (masks[r])
                rows.push_back(op(101, {dst(r, masks[r], 2)}));
        rows.push_back(op(104, {{1}}));
        rows.push_back(op(54, {dst(0, 1), {0x16001}}));
        for (unsigned r = 0; r < 32; ++r)
            if (masks[r])
                rows.push_back(
                    op(54, {dst(r, masks[r], 2), {0xa01006 | (swizzle(masks[r]) << 4), 0x10000a, 0, r}}));
        rows.push_back(op(62));
        phases.insert(phases.begin(), std::move(rows));
    }
    Rows result = globals;
    unsigned writes = 0;
    for (const auto &rows : phases) {
        const bool control = (rows[0][0] & 2047) == 114;
        size_t split = 1;
        while (split < rows.size() && declaration(rows[split][0] & 2047))
            ++split;
        Rows declarations(rows.begin() + 1, rows.begin() + split);
        unsigned temporary = 0, n = 0;
        for (auto &row : declarations)
            if ((row[0] & 2047) == 104) {
                if (++n > 1 || row.size() != 2)
                    throw std::runtime_error("HS phase temp declaration");
                temporary = row[1];
                if (temporary > 4093)
                    throw std::runtime_error("HS phase temporaries exhausted");
                row[1] += 2;
            }
        if (!n)
            declarations.push_back(op(104, {{2}}));
        auto special = [&](unsigned kind) {
            return std::any_of(declarations.begin(), declarations.end(), [&](const Row &r) {
                return (r[0] & 2047) == 95 && r.size() == 2 && ((r[1] >> 12) & 255) == kind;
            });
        };
        if (!special(11))
            declarations.insert(declarations.begin(), op(95, {{0xb000}}));
        if (control && !special(22))
            declarations.insert(declarations.begin(), op(95, {{0x16000}}));
        const auto kind = control ? 1u : 25u;
        if (!identity.is_null())
            declarations.insert(
                declarations.begin(),
                op(95, {{0x200002 | (kind << 12) | (1u << (4 + identity.at("component").get<unsigned>())), cp,
                         identity.at("register").get<unsigned>()}}));
        const auto value = temporary, address = temporary + 1;
        result.push_back(rows[0]);
        result.insert(result.end(), declarations.begin(), declarations.end());
        auto add = [&](unsigned c, std::initializer_list<Row> args) { result.push_back(op(c, args)); };
        for (size_t i = split; i < rows.size(); ++i) {
            const auto &row = rows[i];
            const auto code = row[0] & 2047;
            auto arity = dxbc_detail::operandArities.find(code);
            if (arity == dxbc_detail::operandArities.end())
                throw std::runtime_error("Unsupported HS executable opcode");
            size_t pos = 1;
            for (auto extended = row[0] >> 31; extended;)
                extended = row.at(pos++) >> 31;
            std::vector<std::pair<size_t, size_t>> operands, outputsFound;
            std::set<unsigned> unused;
            for (unsigned a = 0; a < arity->second; ++a) {
                auto end = dxbc_detail::operand(row, pos, unused, 0, false);
                operands.emplace_back(pos, end);
                if (((row[pos] >> 12) & 255) == 2)
                    outputsFound.emplace_back(pos, end);
                pos = end;
            }
            if (pos != row.size())
                throw std::runtime_error("Unsupported HS instruction operand layout");
            if (outputsFound.empty()) {
                result.push_back(row);
                continue;
            }
            if (outputsFound.size() != 1 || outputsFound[0] != operands[0])
                throw std::runtime_error("HS needs a single first output destination");
            auto [a, b] = outputsFound[0];
            Row destination(row.begin() + a, row.begin() + b);
            const auto token = destination[0], mask = (token >> 4) & 15, representation = (token >> 22) & 7;
            if (!mask || (token & 3) != 2 || ((token >> 2) & 3) || ((token >> 20) & 3) != 1 || token >> 31)
                throw std::runtime_error("Unsupported HS output operand shape");
            Row index;
            if (representation == 0)
                index = imm(destination.at(1));
            else if (representation == 2)
                index = Row(destination.begin() + 1, destination.end());
            else if (representation == 3) {
                add(30, {dst(address, 2), Row(destination.begin() + 2, destination.end()),
                         imm(destination.at(1))});
                index = src(address, 1);
            } else
                throw std::runtime_error("Unsupported HS relative output index");
            Row rewritten(row.begin(), row.begin() + a);
            auto temp = dst(value, mask);
            rewritten.insert(rewritten.end(), temp.begin(), temp.end());
            rewritten.insert(rewritten.end(), row.begin() + b, row.end());
            rewritten[0] = (rewritten[0] & ~0x7f000000u) | (uint32_t(rewritten.size()) << 24);
            result.push_back(std::move(rewritten));
            add(54, {destination, {0x100006 | (swizzle(mask) << 4), value}});
            add(35, {dst(address, 1),
                     {0xb001},
                     imm(stride),
                     imm(control ? 0 : m.at("patch_constant_offset").get<unsigned>())});
            if (!identity.is_null())
                add(35, {dst(address, 1),
                         {0x20000a | (kind << 12) | (identity.at("component").get<unsigned>() << 4), 0,
                          identity.at("register").get<unsigned>()},
                         imm(perInstance * stride),
                         src(address, 0)});
            if (control)
                add(35, {dst(address, 1), {0x16001}, imm(m.at("control_point_stride")), src(address, 0)});
            add(35, {dst(address, 1), index, imm(16), src(address, 0)});
            for (unsigned c = 0; c < 4; ++c)
                if (mask & (1u << c)) {
                    add(30, {dst(address, 2), src(address, 0), imm(c * 4)});
                    add(166, {dst(slot, 1, 30), src(address, 1), src(value, c)});
                    add(30, {dst(address, 2), src(address, 1), imm(uint32_t(size))});
                    add(166, {dst(slot, 1, 30), src(address, 1), imm(1)});
                }
            ++writes;
        }
    }
    if (std::none_of(phases.begin(), phases.end(),
                     [](const Rows &rows) { return (rows[0][0] & 2047) == 116; })) {
        result.push_back(op(116));
        result.push_back(op(62));
    }
    p.program.instructions = std::move(result);
    uint64_t flags = 0;
    auto f =
        std::find_if(p.parts.begin(), p.parts.end(), [](const auto &p) { return p.first == 0x30494653; });
    if (f != p.parts.end()) {
        if (f->second.size() != 8)
            throw std::runtime_error("Invalid HS feature flags");
        flags = Reader(f->second).read<uint64_t>();
    }
    flags |= 4 | (slot >= 8 ? 8 : 0);
    std::vector<uint8_t> flagBytes(8);
    std::memcpy(flagBytes.data(), &flags, 8);
    if (f == p.parts.end())
        p.parts.emplace_back(0x30494653, std::move(flagBytes));
    else
        f->second = std::move(flagBytes);
    m.update(Json{{"patches", patches},
                  {"data_bytes", size},
                  {"validity_offset", size},
                  {"total_bytes", size * 2},
                  {"uav_slot", slot},
                  {"mirrored_output_instructions", writes},
                  {"implicit_control_point_phase_materialized", synthesized}});
    return {p.finish(), m};
}
} // namespace flora
