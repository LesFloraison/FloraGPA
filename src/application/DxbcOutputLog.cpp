#include "DxbcOutputLog.h"
#include "DxbcInspection.h"
#include "DxbcOperandArities.h"
#include "core/StreamOutput.h"
#include <algorithm>
#include <bit>
#include <optional>
namespace flora {
namespace {
using Json = nlohmann::json;
using Row = std::vector<uint32_t>;
using Rows = std::vector<Row>;
constexpr uint32_t SHEX = 0x58454853, SHDR = 0x52444853, SFI0 = 0x30494653;
Row op(uint32_t code, std::initializer_list<Row> operands = {}) {
    Row result{code};
    for (const auto &operand : operands)
        result.insert(result.end(), operand.begin(), operand.end());
    if (result.size() > 127)
        throw std::runtime_error("Output log instruction length exceeds DXBC limit");
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Row dst(unsigned reg, unsigned mask = 15, unsigned kind = 0) {
    return {0x100002 | (kind << 12) | (mask << 4), reg};
}
Row src(unsigned reg, unsigned component) { return {0x10000a | (component << 4), reg}; }
Row imm(uint32_t value) { return {0x4001, value}; }
unsigned swizzle(unsigned mask) {
    auto first = std::countr_zero(mask);
    unsigned value = 0;
    for (unsigned c = 0; c < 4; ++c)
        value |= (mask & (1u << c) ? c : first) << (2 * c);
    return value;
}
bool declaration(unsigned opcode, bool gs) {
    return opcode == 53 || (opcode >= 88 && opcode <= 106) || (opcode >= 143 && opcode <= 162) ||
           (gs && opcode == 206);
}
class LogProgram {
  public:
    dxbc_detail::Parts parts;
    DxbcProgram program;
    Rows result;
    Json inputs = Json::array(), outputs = Json::array(), known = Json::object();
    std::vector<std::pair<unsigned, Row>> identities;
    unsigned split = 0, registers = 0, temps = 0, stride = 0, slot, capacity;
    uint32_t key;
    std::string stage, profile;
    LogProgram(Bytes original, unsigned slot_, unsigned capacity_, bool gs)
        : slot(slot_), capacity(capacity_) {
        if (slot >= 64 || !capacity)
            throw std::runtime_error("Output log UAV slot or capacity out of range");
        std::set<uint32_t> tags;
        bool outputFound = false;
        for (const auto &[tag, bytes] : readDxbcParts(original)) {
            if (!tags.insert(tag).second)
                throw std::runtime_error("Duplicate DXBC chunk");
            parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
            if (tag == 0x4e475349)
                inputs = dxbc_detail::signature(bytes);
            if (tag == 0x4e47534f || tag == 0x3547534f) {
                if (outputFound)
                    throw std::runtime_error("Conflicting output signatures");
                outputFound = true;
                outputs = dxbc_detail::signature(bytes, tag == 0x3547534f);
            }
            if (tag == 0x31475349 || tag == 0x3147534f)
                throw std::runtime_error("Output log requires legacy SM4/SM5 signatures");
        }
        key = tags.contains(SHEX) ? SHEX : SHDR;
        auto found = std::find_if(parts.begin(), parts.end(), [&](const auto &v) { return v.first == key; });
        if (found == parts.end())
            throw std::runtime_error("Output log requires an executable shader");
        program = readDxbcProgram(found->second);
        const auto version = program.header[0];
        if (gs ? (version != 0x20040 && version != 0x20041 && version != 0x20050)
               : (version != 0x10040 && version != 0x10041 && version != 0x10050 && version != 0x40050))
            throw std::runtime_error("Unsupported output log shader profile");
        stage = gs ? "gs" : version == 0x40050 ? "ds" : "vs";
        profile = stage + "_" + std::to_string((version >> 4) & 15) + "_" + std::to_string(version & 15);
        if ((version & 255) == 0x50 && dxbc_detail::uavSlots(program, parts).contains(slot))
            throw std::runtime_error("Output log UAV slot collides with an existing declaration");
        const auto masks = dxbc_detail::occupied(outputs, program, 2);
        for (unsigned reg = 0; reg < masks.size(); ++reg)
            if (masks[reg])
                registers = reg + 1;
        if (!gs && !registers)
            throw std::runtime_error("Output write log has no output registers");
        while (split < program.instructions.size() && declaration(program.instructions[split][0] & 2047, gs))
            result.push_back(program.instructions[split++]);
        Row *temp = nullptr;
        for (auto &row : result)
            if ((row[0] & 2047) == 104) {
                if (temp || row.size() != 2)
                    throw std::runtime_error("Invalid or repeated temporary declaration");
                temp = &row;
            }
        temps = temp ? temp->at(1) : 0;
        const auto added = gs ? registers * 2 + 3 : 3;
        if (temps > 4096 - added)
            throw std::runtime_error("Output log exceeds temporary register limit");
        if (temp)
            temp->at(1) += added;
        else
            result.push_back(op(104, {{added}}));
        stride = (gs ? 32 : 16) + registers * 32;
        if (uint64_t(16) + uint64_t(capacity) * stride > 256 * 1024 * 1024)
            throw std::runtime_error("Output log exceeds 256 MiB byte limit");
    }
    void append(uint32_t code, std::initializer_list<Row> operands = {}) {
        result.push_back(op(code, operands));
    }
    void store(unsigned address, unsigned offset, const Row &value) {
        append(30, {dst(address, 2), src(address, 0), imm(offset)});
        append(166, {dst(slot, 1, 30), src(address, 1), value});
    }
    void wrap(unsigned ticket, unsigned reg, unsigned component, unsigned offset) {
        append(80, {dst(ticket, 4), src(reg, component), imm(UINT32_MAX)});
        append(31 | (1 << 18), {src(ticket, 2)});
        append(166, {dst(slot, 1, 30), imm(offset), imm(1)});
        append(21);
    }
    struct Destination {
        size_t begin, end;
        unsigned reg, mask;
        Row words;
    };
    std::optional<Destination> destination(const Row &row) {
        const auto opcode = row[0] & 2047;
        const auto count = dxbc_detail::operandArities.find(opcode);
        if (count == dxbc_detail::operandArities.end())
            throw std::runtime_error("Unsupported output log opcode " + std::to_string(opcode));
        size_t pos = 1;
        for (auto extended = row[0] >> 31; extended;)
            extended = row.at(pos++) >> 31;
        std::optional<Destination> output;
        std::set<unsigned> unused;
        for (unsigned i = 0; i < count->second; ++i) {
            const auto begin = pos;
            pos = dxbc_detail::operand(row, pos, unused, 0, false);
            const auto token = row[begin];
            if (((token >> 12) & 255) != 2)
                continue;
            if (i != 0 || output)
                throw std::runtime_error("Output log requires a single first output destination");
            const auto mask = (token >> 4) & 15;
            if (pos - begin != 2 || (token >> 31) || (token & 3) != 2 || ((token >> 2) & 3) ||
                ((token >> 20) & 3) != 1 || ((token >> 22) & 7) || !mask)
                throw std::runtime_error("Output log requires static masked output destinations");
            const auto reg = row[begin + 1];
            if (reg >= registers)
                throw std::runtime_error("Output write exceeds log register allocation");
            output = Destination{begin, pos, reg, mask, Row(row.begin() + begin, row.begin() + pos)};
        }
        if (pos != row.size())
            throw std::runtime_error("Unsupported output log instruction layout " + std::to_string(opcode));
        return output;
    }
    void mirror(const Row &row, const Destination &output, unsigned reg) {
        auto rewritten = row;
        const auto replacement = dst(reg, output.mask);
        std::copy(replacement.begin(), replacement.end(), rewritten.begin() + output.begin);
        // Static destinations have the same token count; preserve all opcode modifiers.
        result.push_back(std::move(rewritten));
        append(54, {output.words, {0x100006 | (swizzle(output.mask) << 4), reg}});
    }
    OutputLogShader finish(Json extras, unsigned writes) {
        program.header[0] = (program.header[0] & ~255u) | 0x50;
        program.instructions = std::move(result);
        std::erase_if(parts, [&](const auto &p) { return p.first == key; });
        parts.emplace_back(SHEX, writeDxbcProgram(program));
        auto flags = std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == SFI0; });
        if (flags == parts.end()) {
            parts.emplace_back(SFI0, std::vector<uint8_t>(8));
            flags = std::prev(parts.end());
        }
        if (flags->second.size() != 8)
            throw std::runtime_error("Invalid shader feature flags");
        auto value = Reader(flags->second).read<uint64_t>() | 4 | (slot >= 8 ? 8 : 0);
        std::memcpy(flags->second.data(), &value, 8);
        const std::set<uint32_t> stale{0x54415453, 0x47424453, 0x42445053,
                                       0x42444c49, 0x49435253, 0x4e444c49};
        DxbcParts assembled;
        for (const auto &[tag, bytes] : parts)
            if (!stale.contains(tag))
                assembled.emplace_back(tag, bytes);
        Json metadata{{"capacity", capacity},
                      {"record_stride", stride},
                      {"registers", registers},
                      {"total_bytes", 16 + uint64_t(capacity) * stride},
                      {"known_inputs", known},
                      {"signature", outputs},
                      {"mirrored_instructions", writes},
                      {"private_slot", slot},
                      {"inputs_added", false},
                      {"outputs_added", false},
                      {"original_profile", profile},
                      {"helper_profile", stage + "_5_0"},
                      {"shader_stage", stage},
                      {"counter_wrap_checked", stage != "vs"}};
        metadata.update(extras);
        return {makeDxbc(assembled), std::move(metadata)};
    }
};
} // namespace
OutputLogShader instrumentOutputWrites(Bytes original, uint32_t slot, uint32_t capacity) {
    LogProgram p(original, slot, capacity, false);
    const auto value = p.temps, address = value + 1, ticket = value + 2;
    Json domain = nullptr;
    if (p.stage == "vs") {
        for (auto system : {6u, 8u}) {
            Json fields = Json::array();
            for (const auto &field : p.inputs)
                if (field.at("system_value") == system)
                    fields.push_back(field);
            if (fields.size() != 1)
                continue;
            const auto &field = fields[0];
            const auto mask = field.at("mask").get<unsigned>(), reg = field.at("register").get<unsigned>();
            if (mask > 8 || !std::has_single_bit(mask) || !(field.at("used_mask").get<unsigned>() & mask))
                continue;
            const auto &rows = p.program.instructions;
            if (!std::any_of(rows.begin(), rows.end(), [&](const Row &r) {
                    return (r[0] & 2047) == 96 && r.size() == 4 && r[2] == reg && r[3] == system &&
                           ((r[1] >> 4) & mask);
                }))
                continue;
            const auto component = unsigned(std::countr_zero(mask));
            p.known[system == 6 ? "vertex" : "instance"] = {{"register", reg}, {"component", component}};
            p.identities.emplace_back(system == 6 ? 0 : 4, Row{0x10100a | (component << 4), reg});
        }
    } else {
        std::vector<unsigned> domains;
        for (const auto &row : p.result)
            if ((row[0] & 2047) == 149)
                domains.push_back((row[0] >> 11) & 3);
        if (domains.size() != 1 || !domains[0])
            throw std::runtime_error("DS output log requires a known tessellation domain");
        domain = domains[0] == 1 ? "isoline" : domains[0] == 2 ? "tri" : "quad";
        for (const auto &row : p.result) {
            if ((row[0] & 2047) != 95 || row.size() != 2)
                continue;
            const auto token = row[1], kind = (token >> 12) & 255;
            if (kind == 11 && token == 0xb000) {
                p.known["primitive"] = {{"operand", "vPrim"}, {"offset", 0}};
                p.identities.emplace_back(0, Row{0xb001});
            } else if (kind == 28) {
                const auto mask = (token >> 4) & 15, allowed = domains[0] == 2 ? 7u : 3u;
                if ((token >> 20) || (token & 3) != 2 || ((token >> 2) & 3) || !mask || (mask & ~allowed))
                    throw std::runtime_error("Unsupported DS domain point declaration");
                Json components = Json::array();
                for (unsigned c = 0; c < 3; ++c)
                    if (mask & (1u << c)) {
                        components.push_back(c);
                        p.identities.emplace_back(4 + 4 * c, Row{0x1c00a | (c << 4)});
                    }
                p.known["domain"] = {{"operand", "vDomain"}, {"components", components}, {"offset", 4}};
            }
        }
    }
    p.append(157, {{0x11e000, slot}});
    p.append(180, {dst(ticket, 1), {0x11e000, slot}, imm(0), imm(1)});
    if (p.stage == "ds")
        p.wrap(ticket, ticket, 0, 4);
    p.append(79, {dst(ticket, 2), src(ticket, 0), imm(capacity)});
    p.append(35, {dst(address, 1), src(ticket, 0), imm(p.stride), imm(16)});
    p.append(31 | (1 << 18), {src(ticket, 1)});
    for (const auto &[offset, operand] : p.identities)
        p.store(address, offset, operand);
    p.append(21);
    unsigned writes = 0;
    for (size_t i = p.split; i < p.program.instructions.size(); ++i) {
        const auto &row = p.program.instructions[i];
        auto output = p.destination(row);
        if (!output) {
            p.result.push_back(row);
            continue;
        }
        p.mirror(row, *output, value);
        p.append(31 | (1 << 18), {src(ticket, 1)});
        for (unsigned c = 0; c < 4; ++c)
            if (output->mask & (1u << c)) {
                const auto offset = 16 + output->reg * 16 + c * 4;
                p.store(address, offset, src(value, c));
                p.store(address, offset + p.registers * 16, imm(1));
            }
        p.append(21);
        ++writes;
    }
    return p.finish({{"domain", domain}}, writes);
}
OutputLogShader instrumentGeometryEmissions(Bytes original, uint32_t slot, uint32_t capacity,
                                            uint32_t stream) {
    LogProgram p(original, slot, capacity, true);
    const auto topologies = streamOutputTopologies(original);
    if (stream > 3 || !topologies.contains(stream) || topologies.at(stream) < 1 || topologies.at(stream) > 3)
        throw std::runtime_error("GS emission stream is not declared by the original shader");
    const auto values = p.temps, flags = values + p.registers, control = flags + p.registers,
               ticket = control + 1, address = control + 2;
    for (const auto &row : p.result) {
        if ((row[0] & 2047) != 95 || row.size() != 2)
            continue;
        if (row[1] == 0xb000) {
            p.known["primitive"] = {{"operand", "vPrim"}, {"offset", 8}};
            p.identities.emplace_back(8, Row{0xb001});
        } else if (row[1] == 0x25000) {
            p.known["gs_instance"] = {{"operand", "vGSInstanceID"}, {"offset", 12}};
            p.identities.emplace_back(12, Row{0x2500a});
        }
    }
    auto clear = [&] {
        for (unsigned reg = values; reg < flags + p.registers; ++reg)
            p.append(54, {dst(reg), imm(0)});
    };
    p.append(157, {{0x11e000, slot}});
    p.append(180, {dst(control, 1), {0x11e000, slot}, imm(8), imm(1)});
    p.wrap(ticket, control, 0, 12);
    p.append(54, {dst(control, 2), imm(0)});
    clear();
    unsigned emits = 0, cuts = 0, writes = 0;
    const std::map<unsigned, unsigned> emissions{{19, 1}, {9, 2}, {20, 3}, {117, 1}, {118, 2}, {119, 3}};
    for (size_t i = p.split; i < p.program.instructions.size(); ++i) {
        const auto &row = p.program.instructions[i];
        const auto opcode = row[0] & 2047;
        if (auto emit = emissions.find(opcode); emit != emissions.end()) {
            unsigned target = 0;
            if (opcode >= 117) {
                if (row.size() != 3 || row[1] != 0x110000 || row[2] > 3)
                    throw std::runtime_error("Unsupported GS emission stream operand");
                target = row[2];
            } else if (row.size() != 1)
                throw std::runtime_error("Unsupported GS emission instruction layout");
            const auto action = emit->second;
            if (target == stream) {
                p.append(180, {dst(ticket, 1), {0x11e000, slot}, imm(0), imm(1)});
                p.wrap(ticket, ticket, 0, 4);
                p.append(79, {dst(ticket, 2), src(ticket, 0), imm(capacity)});
                p.append(35, {dst(address, 1), src(ticket, 0), imm(p.stride), imm(16)});
                p.append(31 | (1 << 18), {src(ticket, 1)});
                p.store(address, 0, src(control, 0));
                p.store(address, 4, src(control, 1));
                for (const auto &[offset, operand] : p.identities)
                    p.store(address, offset, operand);
                p.store(address, 16, imm(stream));
                p.store(address, 20, imm(action));
                if (action == 1 || action == 3) {
                    for (unsigned reg = 0; reg < p.registers; ++reg)
                        for (unsigned c = 0; c < 4; ++c) {
                            p.store(address, 32 + reg * 16 + 4 * c, src(values + reg, c));
                            p.store(address, 32 + p.registers * 16 + reg * 16 + 4 * c, src(flags + reg, c));
                        }
                    ++emits;
                }
                if (action == 2 || action == 3)
                    ++cuts;
                p.append(21);
                p.append(30, {dst(control, 2), src(control, 1), imm(1)});
            }
            p.result.push_back(row);
            // Emit invalidates every output register, including other streams.
            if (action == 1 || action == 3)
                clear();
            continue;
        }
        auto output = p.destination(row);
        if (!output) {
            p.result.push_back(row);
            continue;
        }
        p.mirror(row, *output, values + output->reg);
        p.append(54, {dst(flags + output->reg, output->mask), imm(1)});
        ++writes;
    }
    std::erase_if(p.outputs.get_ref<Json::array_t &>(),
                  [&](const Json &field) { return field.value("stream", 0u) != stream; });
    const auto topology = topologies.at(stream);
    return p.finish({{"stream", stream},
                     {"topology", topology == 1   ? "pointlist"
                                  : topology == 2 ? "linestrip"
                                                  : "trianglestrip"},
                     {"emit_sites", emits},
                     {"cut_sites", cuts},
                     {"value_offset", 32}},
                    writes);
}
} // namespace flora
