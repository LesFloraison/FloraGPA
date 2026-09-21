#include "DxbcCoverage.h"
#include "DxbcInspection.h"
#include <algorithm>
#include <bit>

namespace flora {
namespace {
using Json = nlohmann::json;
using Words = std::vector<uint32_t>;
using Rows = std::vector<Words>;
constexpr uint32_t ISGN = 0x4e475349, OSGN = 0x4e47534f, SHEX = 0x58454853, SHDR = 0x52444853,
                   RDEF = 0x46454452, SFI0 = 0x30494653;
uint32_t word(Bytes bytes, size_t offset) {
    if (offset > bytes.size())
        throw std::runtime_error("DXBC field bounds");
    return Reader(bytes.subspan(offset)).read<uint32_t>();
}
void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("DXBC patch bounds");
    std::memcpy(bytes.data() + offset, &value, 4);
}
bool declaration(uint32_t code) {
    return code == 53 || (code >= 88 && code <= 106) || (code >= 143 && code <= 162);
}
bool uavType(uint32_t kind) { return kind == 4 || kind == 6 || (kind >= 8 && kind <= 11); }
class Program {
  public:
    dxbc_detail::Parts parts;
    DxbcProgram code;
    uint32_t key;
    explicit Program(Bytes bytes) {
        std::set<uint32_t> tags;
        for (const auto &[tag, payload] : readDxbcParts(bytes)) {
            if (!tags.insert(tag).second)
                throw std::runtime_error("Duplicate DXBC chunk");
            parts.emplace_back(tag, std::vector<uint8_t>(payload.begin(), payload.end()));
        }
        key = contains(SHEX) ? SHEX : SHDR;
        code = readDxbcProgram(part(key));
    }
    bool contains(uint32_t tag) const {
        return std::any_of(parts.begin(), parts.end(), [=](const auto &p) { return p.first == tag; });
    }
    std::vector<uint8_t> &part(uint32_t tag) { return dxbc_detail::part(parts, tag); }
    Json signature(uint32_t tag) { return contains(tag) ? dxbc_detail::signature(part(tag)) : Json::array(); }
    void pixel() const {
        if (code.header[0] != 0x40 && code.header[0] != 0x41 && code.header[0] != 0x50)
            throw std::runtime_error("Coverage requires an SM4/SM5 pixel shader");
        if (!contains(OSGN) || contains(0x3547534f) || contains(0x3147534f))
            throw std::runtime_error("Unsupported pixel output signature variant");
    }
    void appendSignature(uint32_t tag, const std::string &semantic, uint32_t index, uint32_t system,
                         uint32_t type, uint32_t reg, uint8_t mask, uint8_t used) {
        auto &payload = part(tag);
        dxbc_detail::signature(payload);
        const auto count = word(payload, 0), base = 8 + 24 * count;
        std::vector<std::vector<uint8_t>> rows;
        for (uint32_t i = 0; i < count; ++i) {
            std::vector<uint8_t> row(payload.begin() + 8 + 24 * i, payload.begin() + 32 + 24 * i);
            put(row, 0, word(row, 0) + 24);
            rows.push_back(std::move(row));
        }
        if (payload.size() > UINT32_MAX - 24)
            throw std::runtime_error("DXBC signature exceeds uint32 size");
        std::vector<uint8_t> row(24);
        put(row, 0, uint32_t(payload.size() + 24));
        put(row, 4, index);
        put(row, 8, system);
        put(row, 12, type);
        put(row, 16, reg);
        row[20] = mask;
        row[21] = used;
        rows.push_back(std::move(row));
        std::stable_sort(rows.begin(), rows.end(),
                         [](const auto &a, const auto &b) { return word(a, 16) < word(b, 16); });
        std::vector<uint8_t> result(payload.begin(), payload.begin() + 8);
        put(result, 0, count + 1);
        for (const auto &r : rows)
            result.insert(result.end(), r.begin(), r.end());
        result.insert(result.end(), payload.begin() + base, payload.end());
        result.insert(result.end(), semantic.begin(), semantic.end());
        result.resize(result.size() + 4 - semantic.size() % 4, 0);
        payload = std::move(result);
    }
    std::vector<uint8_t> finish(bool removeStatistics) {
        part(key) = writeDxbcProgram(code);
        std::erase_if(parts, [=](const auto &p) {
            return (removeStatistics && p.first == 0x54415453) || p.first == 0x47424453 ||
                   p.first == 0x42445053 || p.first == 0x42444c49 || p.first == 0x49435253 ||
                   p.first == 0x4e444c49;
        });
        DxbcParts output;
        for (const auto &[tag, data] : parts)
            output.emplace_back(tag, data);
        return makeDxbc(output);
    }
};
uint32_t uintField(const Json &value) {
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0) ||
        value.get<uint64_t>() > UINT32_MAX)
        throw std::runtime_error("Invalid coverage register field");
    return value.get<uint32_t>();
}
Rows arrayMarker(Program &p, const CoverageMarkerOptions &o) {
    const auto index = *o.arrayIndex;
    if (!o.arrayRouted)
        return {{0x08000036, 0x001020f2, o.slot, 0x00004002, index == 0 ? 0x3f800000u : 0u,
                 index == 0 ? 0x3f800000u : 0u, index == 0 ? 0x3f800000u : 0u,
                 index == 0 ? 0x3f800000u : 0u}};
    if (!p.contains(ISGN) || p.contains(0x31475349) || p.contains(0x35475349))
        throw std::runtime_error("Viewport index selection requires an SM4/SM5 input signature");
    const auto inputs = p.signature(ISGN);
    Json existing = Json::array();
    for (const auto &s : inputs)
        if (s.at("system_value") == 4)
            existing.push_back(s);
    uint32_t reg, mask;
    Rows declarations;
    if (!existing.empty()) {
        if (existing.size() != 1)
            throw std::runtime_error("Ambiguous render-target array index input");
        const auto &item = existing[0];
        reg = uintField(item.at("register"));
        mask = uintField(item.at("mask"));
        if (item.at("component_type") != 1 || !std::has_single_bit(mask) || mask > 8)
            throw std::runtime_error("Invalid render-target array index input");
    } else {
        if (o.arraySource.is_null())
            throw std::runtime_error("Viewport index selection needs the rasterizer producer output layout");
        reg = uintField(o.arraySource.at("register"));
        mask = uintField(o.arraySource.at("mask"));
        if (reg >= 32 || !std::has_single_bit(mask) || mask > 8 || o.arraySource.at("component_type") != 1)
            throw std::runtime_error("Invalid producer array index output");
        for (const auto &s : inputs)
            if (s.at("register") == reg && (uintField(s.at("mask")) & mask))
                throw std::runtime_error("Viewport index input overlaps the original PS input layout");
        declarations.push_back({0x04000864, 0x00101002 | (mask << 4), reg, 4});
        p.appendSignature(ISGN, "SV_RenderTargetArrayIndex", 0, 4, 1, reg, uint8_t(mask), uint8_t(mask));
    }
    Words *temps = nullptr;
    for (auto &row : p.code.instructions)
        if ((row[0] & 2047) == 104) {
            if (temps || row.size() != 2)
                throw std::runtime_error("Unsupported temporary register declaration");
            temps = &row;
        }
    const auto temporary = temps ? (*temps)[1] : 0;
    if (temporary >= 4096)
        throw std::runtime_error("Viewport index selection needs a free temporary register");
    if (temps)
        ++(*temps)[1];
    else
        declarations.push_back({0x02000068, 1});
    auto split = std::find_if_not(p.code.instructions.begin(), p.code.instructions.end(),
                                  [](const auto &r) { return declaration(r[0] & 2047); });
    p.code.instructions.insert(split, declarations.begin(), declarations.end());
    return {{0x07000020, 0x00100012, temporary, 0x0010100au | (uint32_t(std::countr_zero(mask)) << 4), reg,
             0x00004001, index},
            {0x0a000001, 0x001020f2, o.slot, 0x00100006, temporary, 0x00004002, 0x3f800000, 0x3f800000,
             0x3f800000, 0x3f800000}};
}
size_t relocateOperand(Words &row, size_t pos, const std::map<uint32_t, uint32_t> &mapping,
                       std::set<uint32_t> &found, unsigned depth = 0) {
    if (depth > 32 || pos >= row.size())
        throw std::runtime_error("Invalid DXBC operand bounds");
    const auto token = row[pos++], kind = (token >> 12) & 255, dimensions = (token >> 20) & 3;
    for (auto extended = token >> 31; extended;)
        extended = row.at(pos++) >> 31;
    if (kind == 30) {
        if (dimensions != 1 || ((token >> 22) & 7))
            throw std::runtime_error("UAV relocation requires static SM5 registers");
        auto &slot = row.at(pos);
        found.insert(slot);
        if (auto it = mapping.find(slot); it != mapping.end())
            slot = it->second;
    }
    for (unsigned d = 0; d < dimensions; ++d) {
        const auto repr = (token >> (22 + 3 * d)) & 7;
        if (repr == 0 || repr == 3)
            ++pos;
        else if (repr == 1 || repr == 4)
            pos += 2;
        else if (repr != 2)
            throw std::runtime_error("Unsupported DXBC index representation");
        if (repr >= 2 && repr <= 4)
            pos = relocateOperand(row, pos, mapping, found, depth + 1);
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
void validateSlots(uint32_t count) {
    if (count != 8 && count != 64)
        throw std::runtime_error("UAV slot count must be 8 or 64");
}
} // namespace
std::vector<uint8_t> addCoverageMarker(Bytes original, uint32_t slot) {
    Program p(original);
    p.pixel();
    if (slot >= 8)
        throw std::runtime_error("Coverage target slot must be 0..7");
    for (const auto &s : p.signature(OSGN))
        if (s.at("register") == slot)
            throw std::runtime_error("Coverage slot already used by shader");
    const Rows marker{
        {0x03000065, 0x001020f2, slot},
        {0x08000036, 0x001020f2, slot, 0x00004002, 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000}};
    auto split = std::find_if_not(p.code.instructions.begin(), p.code.instructions.end(),
                                  [](const auto &r) { return declaration(r[0] & 2047); });
    p.code.instructions.insert(split, marker.begin(), marker.end());
    p.appendSignature(OSGN, "SV_Target", slot, 64, 3, slot, 15, 0);
    return p.finish(true);
}
std::vector<uint8_t> replaceCoverageMarker(Bytes original, const CoverageMarkerOptions &o) {
    if (o.slot >= 8)
        throw std::runtime_error("Coverage target slot must be 0..7");
    Program initial(original);
    initial.pixel();
    const auto outputs = initial.signature(OSGN);
    std::vector<uint8_t> withOutput;
    if (std::none_of(outputs.begin(), outputs.end(),
                     [&](const auto &s) { return s.at("register") == o.slot; }))
        withOutput = addCoverageMarker(original, o.slot);
    Program p(withOutput.empty() ? original : Bytes(withOutput));
    Rows marker{{0x08000036, o.keepAlpha ? 0x00102072u : 0x001020f2u, o.slot, 0x00004002, 0x3f800000,
                 0x3f800000, 0x3f800000, 0x3f800000}};
    if (o.arrayIndex) {
        if (o.keepAlpha)
            throw std::runtime_error("Targetless index selection cannot preserve target alpha");
        marker = arrayMarker(p, o);
    }
    Rows changed;
    bool main = true;
    for (auto row : p.code.instructions) {
        const auto op = row[0] & 2047;
        if (op == 44)
            main = false;
        if (op == 101 && row.size() == 3 && row[2] == o.slot)
            row[1] = 0x001020f2;
        if (main && (op == 62 || op == 63))
            changed.insert(changed.end(), marker.begin(), marker.end());
        changed.push_back(std::move(row));
    }
    p.code.instructions = std::move(changed);
    auto &signature = p.part(OSGN);
    const auto count = word(signature, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const auto offset = 8 + 24 * i;
        if (word(signature, offset + 16) == o.slot) {
            put(signature, offset + 12, 3);
            signature.at(offset + 20) = 15;
            signature.at(offset + 21) = 0;
        }
    }
    return p.finish(true);
}
RelocatedShader relocateShaderUavs(Bytes original, const std::map<uint32_t, uint32_t> &mapping,
                                   uint32_t slotCount) {
    validateSlots(slotCount);
    for (const auto &[from, to] : mapping)
        if (from >= slotCount || to >= slotCount)
            throw std::runtime_error("UAV relocation slot exceeds device limit");
    Program p(original);
    if ((p.code.header[0] & 0xffff) != 0x50 || (p.code.header[0] >> 16) > 5)
        throw std::runtime_error("UAV relocation requires an SM5 shader");
    const std::map<uint32_t, unsigned> counts{
        {61, 3},  {121, 2}, {156, 1}, {157, 1}, {158, 1}, {163, 3}, {164, 3}, {165, 3},
        {166, 3}, {167, 4}, {168, 4}, {169, 3}, {170, 3}, {171, 3}, {172, 4}, {173, 3},
        {174, 3}, {175, 3}, {176, 3}, {177, 3}, {178, 2}, {179, 2}, {180, 4}, {181, 4},
        {182, 4}, {183, 4}, {184, 4}, {185, 5}, {186, 4}, {187, 4}, {188, 4}, {189, 4}};
    std::set<uint32_t> declared, used;
    for (auto &row : p.code.instructions) {
        const auto op = row[0] & 2047;
        if (op >= 225 && op <= 227)
            throw std::runtime_error("UAV feedback relocation is not implemented");
        auto count = counts.find(op);
        if (count == counts.end())
            continue;
        size_t pos = 1;
        for (auto extended = row[0] >> 31; extended;)
            extended = row.at(pos++) >> 31;
        std::set<uint32_t> found;
        for (unsigned i = 0; i < count->second; ++i)
            pos = relocateOperand(row, pos, mapping, found);
        if (pos + (op == 156 || op == 158 ? 1 : 0) != row.size())
            throw std::runtime_error("Unexpected UAV instruction operand layout");
        used.insert(found.begin(), found.end());
        if (op >= 156 && op <= 158)
            declared.insert(found.begin(), found.end());
    }
    for (auto slot : used)
        if (!declared.contains(slot))
            throw std::runtime_error("UAV use lacks a declaration");
    std::set<uint32_t> destinations;
    for (auto slot : declared) {
        if (slot >= slotCount)
            throw std::runtime_error("Declared UAV exceeds device limit");
        const auto target = mapping.contains(slot) ? mapping.at(slot) : slot;
        if (!destinations.insert(target).second)
            throw std::runtime_error("UAV relocation collides with an existing register");
    }
    if (!destinations.empty() && *destinations.rbegin() >= 8) {
        if (!p.contains(SFI0))
            p.parts.emplace_back(SFI0, std::vector<uint8_t>(8));
        auto &flags = p.part(SFI0);
        if (flags.size() != 8)
            throw std::runtime_error("Invalid shader feature flags");
        put(flags, 0, word(flags, 0) | 8);
    }
    if (p.contains(RDEF)) {
        auto &reflection = p.part(RDEF);
        const auto count = word(reflection, 8), offset = word(reflection, 12);
        if (offset > reflection.size() || uint64_t(count) * 32 > reflection.size() - offset)
            throw std::runtime_error("Reflected UAV bounds");
        for (uint32_t i = 0; i < count; ++i) {
            const auto row = size_t(offset) + i * 32;
            if (!uavType(word(reflection, row + 4)))
                continue;
            const auto slot = word(reflection, row + 20), size = word(reflection, row + 24);
            if (!size || slot >= slotCount || size > slotCount - slot)
                throw std::runtime_error("Invalid SM5 reflected UAV register range");
            const auto first = mapping.contains(slot) ? mapping.at(slot) : slot;
            for (uint32_t n = 0; n < size; ++n)
                if ((mapping.contains(slot + n) ? mapping.at(slot + n) : slot + n) != first + n)
                    throw std::runtime_error("UAV relocation would split a reflected register range");
            put(reflection, row + 20, first);
        }
    }
    return {p.finish(false), declared};
}
ReservedCoverageTarget reserveCoverageTarget(Bytes original, const std::set<uint32_t> &boundSlots,
                                             uint32_t slotCount) {
    validateSlots(slotCount);
    Program p(original);
    auto occupied = boundSlots;
    if (p.contains(RDEF)) {
        const auto &r = p.part(RDEF);
        const auto count = word(r, 8), offset = word(r, 12);
        if (offset > r.size() || uint64_t(count) * 32 > r.size() - offset)
            throw std::runtime_error("Reflected UAV bounds");
        for (uint32_t i = 0; i < count; ++i) {
            const auto row = size_t(offset) + i * 32;
            if (!uavType(word(r, row + 4)))
                continue;
            const auto slot = word(r, row + 20), size = word(r, row + 24);
            if (slot >= slotCount || size > slotCount - slot)
                throw std::runtime_error("PS UAV slot exceeds device limit");
            for (uint32_t n = 0; n < size; ++n)
                occupied.insert(slot + n);
        }
    }
    if (p.code.header[0] == 0x50) {
        const auto tokens = relocateShaderUavs(original, {}, slotCount).declared;
        occupied.insert(tokens.begin(), tokens.end());
    }
    for (auto slot : occupied)
        if (slot >= slotCount)
            throw std::runtime_error("PS UAV slot exceeds device limit");
    std::map<uint32_t, uint32_t> mapping;
    if (occupied.contains(0)) {
        auto shift = [&] {
            for (auto slot : occupied)
                mapping[slot] = slot + 1;
        };
        if (*occupied.rbegin() < 7)
            shift();
        else {
            uint32_t free = 1;
            while (free < 8 && occupied.contains(free))
                ++free;
            if (free < 8)
                mapping[0] = free;
            else if (*occupied.rbegin() < slotCount - 1)
                shift();
            else {
                free = 8;
                while (free < slotCount && occupied.contains(free))
                    ++free;
                if (free == slotCount)
                    throw std::runtime_error("Coverage needs one free PS UAV slot to relocate u0");
                mapping[0] = free;
            }
        }
    }
    return {mapping.empty() ? std::vector<uint8_t>(original.begin(), original.end())
                            : relocateShaderUavs(original, mapping, slotCount).bytes,
            mapping};
}
} // namespace flora
