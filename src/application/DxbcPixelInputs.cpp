#include "DxbcPixelInputs.h"
#include "DxbcOperandArities.h"
#include "HlslCompilation.h"
#include "core/Dxbc.h"
#include <algorithm>
#include <bit>
#include <set>

namespace flora::dxbc_exec {
namespace {
using Row = std::vector<uint32_t>;
constexpr uint32_t ISGN = 0x4e475349, ISG1 = 0x31475349, OSGN = 0x4e47534f, SHDR = 0x52444853,
                   SHEX = 0x58454853;
uint32_t word(Bytes bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Pixel input signature bounds");
    return Reader(bytes.subspan(offset)).read<uint32_t>();
}
void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("Pixel input signature write bounds");
    std::memcpy(bytes.data() + offset, &value, 4);
}
struct Field {
    std::string name;
    uint32_t semantic, system, type, reg, mask, used, precision;
};
std::vector<Field> signature(Bytes bytes, uint32_t tag) {
    const auto count = word(bytes, 0);
    const size_t stride = tag == ISG1 ? 32 : 24;
    if (count > 128 || bytes.size() < 8 || uint64_t(count) * stride > bytes.size() - 8)
        throw std::runtime_error("Pixel input signature row bounds");
    std::vector<Field> fields;
    std::map<uint32_t, uint32_t> masks;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t base = 8 + i * stride + (tag == ISG1 ? 4 : 0);
        const auto name = word(bytes, base), reg = word(bytes, base + 16);
        const auto mask = bytes[base + 20], used = bytes[base + 21];
        const auto type = word(bytes, base + 12);
        const auto precision = tag == ISG1 ? word(bytes, base + 24) : 0;
        if (name < 8 + count * stride || name >= bytes.size() ||
            std::find(bytes.begin() + name, bytes.end(), 0) == bytes.end() ||
            (reg >= 32 && reg != UINT32_MAX) || !mask || mask > 15 || (used & ~mask) || type < 1 ||
            type > 3 || (reg != UINT32_MAX && (masks[reg] & mask)) ||
            (tag == ISG1 && word(bytes, base - 4)) ||
            (precision != 0 && precision != 1 && precision != 2 && precision != 4 && precision != 5))
            throw std::runtime_error("Invalid pixel input signature row");
        masks[reg] |= mask;
        fields.push_back({reinterpret_cast<const char *>(bytes.data() + name), word(bytes, base + 4),
                          word(bytes, base + 8), type, reg, mask, used, precision});
    }
    return fields;
}
std::vector<uint8_t> signature(const std::vector<Field> &fields) {
    std::vector<uint8_t> result(8 + fields.size() * 32);
    put(result, 0, uint32_t(fields.size()));
    put(result, 4, 8);
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto &f = fields[i];
        const auto base = 12 + i * 32;
        put(result, base, uint32_t(result.size()));
        put(result, base + 4, f.semantic);
        put(result, base + 8, f.system);
        put(result, base + 12, f.type);
        put(result, base + 16, f.reg);
        result[base + 20] = uint8_t(f.mask);
        result[base + 21] = uint8_t(f.used);
        put(result, base + 24, f.precision);
        result.insert(result.end(), f.name.begin(), f.name.end());
        result.push_back(0);
    }
    while (result.size() % 4)
        result.push_back(0);
    return result;
}
bool declaration(uint32_t op) { return (op >= 88 && op <= 106) || (op >= 143 && op <= 162); }
struct PixelProgram {
    DxbcProgram program;
    std::vector<Field> inputs;
};
PixelProgram readPixelProgram(Bytes original) {
    Bytes input, code;
    uint32_t inputTag = 0;
    std::set<uint32_t> tags;
    for (const auto &[tag, bytes] : readDxbcParts(original)) {
        if (!tags.insert(tag).second)
            throw std::runtime_error("Duplicate pixel input DXBC chunk");
        if (tag == ISGN || tag == ISG1) {
            if (inputTag)
                throw std::runtime_error("Conflicting pixel input signatures");
            inputTag = tag;
            input = bytes;
        }
        if (tag == SHDR || tag == SHEX) {
            if (!code.empty())
                throw std::runtime_error("Conflicting pixel input programs");
            code = bytes;
        }
    }
    if (!inputTag || code.empty())
        throw std::runtime_error("Pixel input capture requires a signature and executable");
    auto program = readDxbcProgram(code);
    if (program.header[0] != 0x40 && program.header[0] != 0x41 && program.header[0] != 0x50)
        throw std::runtime_error("Pixel input capture requires an SM4/SM5 pixel shader");
    return {std::move(program), signature(input, inputTag)};
}
// Only the independently compiled template is rewritten. Walk framed operands
// so immediate data equal to a register token cannot be accidentally changed.
size_t rewrite(Row &row, size_t pos, const std::map<uint32_t, uint32_t> &temps, unsigned depth = 0) {
    if (pos >= row.size() || depth > 16)
        throw std::runtime_error("Pixel input template operand bounds");
    const auto start = pos;
    const auto token = row[pos++];
    const auto kind = (token >> 12) & 255, dimensions = (token >> 20) & 3;
    for (auto ext = token >> 31; ext;)
        ext = row.at(pos++) >> 31;
    if (kind == 1) {
        if (dimensions != 1 || ((token >> 22) & 7))
            throw std::runtime_error("Unexpected relative pixel template input");
        row[start] &= ~(255u << 12);
        row.at(pos) = temps.at(row.at(pos));
    }
    for (unsigned d = 0; d < dimensions; ++d) {
        const auto representation = (token >> (22 + d * 3)) & 7;
        if (representation == 0 || representation == 3)
            ++pos;
        else if (representation == 1 || representation == 4)
            pos += 2;
        else if (representation != 2)
            throw std::runtime_error("Unexpected pixel template index representation");
        if (representation >= 2 && representation <= 4)
            pos = rewrite(row, pos, temps, depth + 1);
    }
    if (kind == 4) {
        if ((token & 3) != 1 && (token & 3) != 2)
            throw std::runtime_error("Unexpected pixel template immediate");
        pos += (token & 3) == 1 ? 1 : 4;
    } else if (kind == 5)
        throw std::runtime_error("Unexpected double pixel template immediate");
    if (pos > row.size())
        throw std::runtime_error("Truncated pixel template operand");
    return pos;
}
const char *body = R"(
#if RECORD_MODE
RWByteAddressBuffer records:register(u1);
#if INDEX_RECORDS
#define RETURN_TYPE uint
#define RETURN_SEMANTIC :SV_Target
#else
#define RETURN_TYPE void
#define RETURN_SEMANTIC
#endif
#else
#define RETURN_TYPE Output
#define RETURN_SEMANTIC
#endif
uint4 transport(uint4 value, float2 role, uint target) {
    float2 delta = float2(target & 1u, target >> 1u) - role;
    uint4 result = 0;
    [unroll] for (uint shift = 0; shift < 32; shift += 8) {
        float4 b = float4((value >> shift) & 255u);
        float4 dx = ddx_fine(b), dy = ddy_fine(b), dxy = ddx_fine(dy);
        result |= uint4(b + delta.x*dx + delta.y*dy + (delta.x*delta.y)*dxy) << shift;
    }
    return result;
}
struct Output {
    uint4 a:SV_Target0; uint4 b:SV_Target1; uint4 c:SV_Target2; uint4 d:SV_Target3;
    uint4 location:SV_Target4; uint4 samples:SV_Target5;
    uint4 coverage:SV_Target6; uint4 orientation:SV_Target7;
};
RETURN_TYPE main(float4 source:FLORA_VALUE, float4 position:SV_Position,
            uint coverage:SV_Coverage SAMPLE_ARGUMENT LAYER_ARGUMENT FREQUENCY_ARGUMENT
#if RECORD_MODE
            , bool front:SV_IsFrontFace
#endif
            ) RETURN_SEMANTIC {
#if RECORD_MODE
#if KEEP_FREQUENCY_TRIGGER
    // Retain a real original sample-frequency attribute even when another
    // register is selected. The private header retains its opaque bit union.
    uint unusedFrequency;
    records.InterlockedOr(12,asuint(frequencyTrigger.x),unusedFrequency);
#endif
    uint record=MAX_RECORDS;
    [branch] if(all(asuint(int2(floor(position.xy)))==uint2(SELECT_X,SELECT_Y)) &&
                LAYER_VALUE==SELECT_LAYER && (coverage&(1u<<SELECT_SAMPLE))!=0 && SAMPLE_MATCH)
        records.InterlockedAdd(0,1,record);
#endif
    float2 parity = float2(int2(floor(position.xy)) & 1);
    float2 direction = float2(ddx_fine(parity.x), ddy_fine(parity.y)) * (1-2*parity);
    float2 role = float2(direction.x < 0, direction.y < 0);
    uint4 value = asuint(source);
    uint4 samples = SAMPLE_VALUE.xxxx;
    Output o;
    o.a=transport(value,role,0); o.b=transport(value,role,1);
    o.c=transport(value,role,2); o.d=transport(value,role,3);
    o.location=uint4(asuint(int2(floor(position.xy))),uint2(role));
    o.samples=uint4(transport(samples,role,0).x,transport(samples,role,1).x,
                    transport(samples,role,2).x,transport(samples,role,3).x);
    o.coverage=uint4(transport(coverage.xxxx,role,0).x,transport(coverage.xxxx,role,1).x,
                     transport(coverage.xxxx,role,2).x,transport(coverage.xxxx,role,3).x);
    o.orientation=asuint(float4(direction,ddx_fine(parity.y),ddy_fine(parity.x)));
#if RECORD_MODE
    // Raw UAV stores outside the view are discarded. Keeping the stores
    // unconditional prevents the compiler sinking derivatives into a pixel
    // selection branch. The allocation counter still runs only for selection.
            uint address=16+144*min(record,MAX_RECORDS);
            records.Store4(address,o.a);records.Store4(address+16,o.b);
            records.Store4(address+32,o.c);records.Store4(address+48,o.d);
            records.Store4(address+64,o.location);records.Store4(address+80,o.samples);
            records.Store4(address+96,o.coverage);records.Store4(address+112,o.orientation);
            records.Store4(address+128,uint4(LAYER_VALUE,SELECT_SAMPLE,asuint(position.z),front?1u:0u));
#if INDEX_RECORDS
            return record+1u;
#endif
#else
    return o;
#endif
})";
} // namespace
std::vector<PixelInputBinding> pixelShaderInputSignature(Bytes original) {
    std::vector<PixelInputBinding> result;
    for (const auto &field : readPixelProgram(original).inputs)
        result.push_back({field.name, field.semantic, field.system, field.type, field.reg, field.mask,
                          field.used, field.precision});
    return result;
}
static PixelInputShader buildPixelInputShader(Bytes original, std::optional<uint32_t> inputRegister,
                                              std::optional<PixelInputRecordSelection> records,
                                              std::optional<PixelAttributeEvaluation> evaluation = {}) {
    if (evaluation && (!inputRegister || evaluation->opcode < 203 || evaluation->opcode > 205 ||
                       (evaluation->opcode == 204 && evaluation->location[1]) ||
                       (evaluation->opcode == 205 && evaluation->location != std::array<uint32_t, 2>{})))
        throw std::runtime_error("Invalid pixel attribute evaluation request");
    if (inputRegister && *inputRegister >= 32)
        throw std::runtime_error("Pixel input register is outside v0..v31");
    if (records && (!records->maxRecords || records->maxRecords > 4096 || records->sample >= 32))
        throw std::runtime_error("Invalid pixel input record limits/sample");
    const auto parsed = readPixelProgram(original);
    const auto &originalProgram = parsed.program;
    PixelInputShader result;
    result.originalShaderSha256 = sha256(original);
    result.reg = inputRegister.value_or(UINT32_MAX);
    result.records = records;
    result.evaluation = evaluation;
    std::vector<Row> selectedDeclarations;
    std::optional<Row> originalPositionDeclaration;
    std::optional<Row> originalFrequencyDeclaration;
    bool minimumPrecision = false, explicitSampleIndex = false, selectedFrequency = false;
    for (const auto &row : originalProgram.instructions) {
        const auto op = row[0] & 2047;
        if (op < 98 || op > 100)
            continue;
        if (row.size() < 2)
            throw std::runtime_error("Truncated pixel input declaration");
        if (((row[1] >> 12) & 255) != 1)
            continue; // Special inputs are observed separately by the helper.
        if ((row[0] & 0x80000000u) || (row[1] & 0x7fffff0fu) != 0x101002)
            throw std::runtime_error("Unsupported pixel input declaration");
        size_t index = 2;
        if (row[1] & 0x80000000u) {
            if (row.size() < 4 || (row[2] & ~0x1c001u) || (row[2] & 63) != 1)
                throw std::runtime_error("Unsupported pixel input operand extension");
            const auto precision = (row[2] >> 14) & 7;
            if (precision == 3 || precision >= 6)
                throw std::runtime_error("Reserved pixel input minimum precision");
            ++index;
        }
        if (row.size() != index + 1 + (op != 98) || row[index] >= 32)
            throw std::runtime_error("Invalid pixel input declaration length/register");
        const auto interpolation = (row[0] >> 11) & 15;
        result.sampleFrequency |= interpolation == 6 || interpolation == 7 || (op != 98 && row.back() == 10);
        if ((interpolation == 6 || interpolation == 7) && !originalFrequencyDeclaration)
            originalFrequencyDeclaration = row;
        explicitSampleIndex |= op != 98 && row.back() == 10;
        if (op == 100 && row.back() == 1) {
            if (originalPositionDeclaration) {
                auto previous = *originalPositionDeclaration, current = row;
                previous[1] &= ~0xf0u;
                current[1] &= ~0xf0u;
                if (previous != current)
                    throw std::runtime_error("Conflicting pixel position declarations");
            }
            originalPositionDeclaration = row;
            minimumPrecision |= index == 3;
        }
        if (!inputRegister || row[index] != *inputRegister)
            continue;
        selectedFrequency |= interpolation == 6 || interpolation == 7;
        const auto mask = (row[1] >> 4) & 15;
        if (!mask || (result.mask & mask))
            throw std::runtime_error("Overlapping or empty pixel input declaration");
        result.mask |= mask;
        if (interpolation == 1)
            result.attributeConstantMask |= mask;
        minimumPrecision |= index == 3;
        auto remapped = row;
        remapped[index] = 0;
        selectedDeclarations.push_back(std::move(remapped));
    }
    if (inputRegister && !result.mask)
        throw std::runtime_error("Pixel input register is not declared");
    std::vector<Field> fields;
    uint32_t signatureMask = 0;
    std::optional<Field> position, sample, layer, front;
    for (auto f : parsed.inputs) {
        if (!inputRegister || f.reg != *inputRegister)
            continue;
        f.reg = 0;
        f.used = f.mask & result.mask;
        signatureMask |= f.mask;
        if (f.system == 1)
            position = f;
        if (f.system == 10)
            sample = f;
        if (f.system == 4)
            layer = f;
        if (f.system == 9)
            front = f;
        fields.push_back(std::move(f));
    }
    if (result.mask & ~signatureMask)
        throw std::runtime_error("Pixel input declaration exceeds signature");
    if (evaluation && position)
        throw std::runtime_error("Pixel attribute evaluation cannot read SV_Position");
    if (position && (position->type != 3 || (position->mask & 3) != 3))
        throw std::runtime_error("Pixel position signature lacks float xy");
    if (sample && (sample->type != 1 || !std::has_single_bit(sample->mask)))
        throw std::runtime_error("Invalid pixel sample-index signature");
    if (layer && (layer->type != 1 || !std::has_single_bit(layer->mask)))
        throw std::runtime_error("Invalid pixel render-target array-index signature");
    if (front && (front->type != 1 || !std::has_single_bit(front->mask)))
        throw std::runtime_error("Invalid pixel front-face signature");
    // A selected position register may only read z/w in the original shader.
    // Add xy for orientation and z for record depth without making added
    // components valid original shader register observations.
    if (position) {
        bool expanded = false;
        for (auto &row : selectedDeclarations)
            if ((row[0] & 2047) == 100 && row.back() == 1) {
                row[1] |= (records ? 7u : 3u) << 4;
                expanded = true;
                break;
            }
        if (!expanded)
            throw std::runtime_error("Pixel position declaration is missing");
        for (auto &f : fields)
            if (f.system == 1) {
                f.used |= records ? 7 : 3;
                f.mask |= records ? 7 : 3;
            }
    }
    result.sampleRecordTarget = records && result.sampleFrequency && !explicitSampleIndex;
    const auto positionInterpolation =
        originalPositionDeclaration ? ((*originalPositionDeclaration)[0] >> 11) & 15 : 0;
    const bool keepFrequency = result.sampleRecordTarget && !selectedFrequency &&
                               positionInterpolation != 6 && positionInterpolation != 7;
    if (keepFrequency && !originalFrequencyDeclaration)
        throw std::runtime_error("Missing original sample-frequency trigger");
    const bool sampleIndex = result.sampleFrequency && !result.sampleRecordTarget;
    std::string defines =
        result.sampleRecordTarget ? "#define INDEX_RECORDS 1\n" : "#define INDEX_RECORDS 0\n";
    if (records) {
        for (const auto &[name, value] : std::initializer_list<std::pair<const char *, uint32_t>>{
                 {"SELECT_X", records->x},
                 {"SELECT_Y", records->y},
                 {"SELECT_SAMPLE", records->sample},
                 {"SELECT_LAYER", records->layer},
                 {"MAX_RECORDS", records->maxRecords * (result.sampleRecordTarget ? 32u : 1u)}})
            defines += "#define " + std::string(name) + " " + std::to_string(value) + "u\n";
        defines += "#define RECORD_MODE 1\n";
        defines +=
            sampleIndex ? "#define SAMPLE_MATCH (sample==SELECT_SAMPLE)\n" : "#define SAMPLE_MATCH true\n";
    } else
        defines += "#define RECORD_MODE 0\n";
    defines +=
        records && records->layerInput
            ? "#define LAYER_ARGUMENT , uint layer:SV_RenderTargetArrayIndex\n#define LAYER_VALUE layer\n"
            : "#define LAYER_ARGUMENT\n#define LAYER_VALUE (0u)\n";
    defines += keepFrequency ? "#define KEEP_FREQUENCY_TRIGGER 1\n#define FREQUENCY_ARGUMENT ,float4 "
                               "frequencyTrigger:FLORA_FREQUENCY\n"
                             : "#define KEEP_FREQUENCY_TRIGGER 0\n#define FREQUENCY_ARGUMENT\n";
    const std::string source =
        defines +
        std::string(sampleIndex ? "#define SAMPLE_ARGUMENT , uint sample:SV_SampleIndex\n"
                                  "#define SAMPLE_VALUE sample\n"
                                : "#define SAMPLE_ARGUMENT\n#define SAMPLE_VALUE (0u)\n") +
        body;
    const auto compiled = compileHlsl(source, "ps_5_0", "main", "Flora pixel input transport").bytecode;
    DxbcProgram program{};
    std::vector<Field> templateFields;
    Bytes output;
    for (const auto &[tag, bytes] : readDxbcParts(compiled)) {
        if (tag == ISGN || tag == ISG1)
            templateFields = signature(bytes, tag);
        if (tag == SHDR || tag == SHEX)
            program = readDxbcProgram(bytes);
        if (tag == OSGN)
            output = bytes;
    }
    uint32_t temporary = 0;
    for (auto &row : program.instructions)
        if ((row[0] & 2047) == 104) {
            temporary = row.at(1);
            row.at(1) += 5;
        }
    if (!temporary || temporary > 4091 || output.empty())
        throw std::runtime_error("Unexpected pixel input transport template");
    std::map<uint32_t, uint32_t> remap;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> helperRegisters;
    uint32_t helperTemps = 1;
    uint32_t nextRegister = inputRegister ? 1 : 0;
    std::vector<Row> initialize{{0x05000036, 0x1000f2, temporary, 0x4001, 0}};
    if (inputRegister && !evaluation)
        initialize.push_back({0x05000036, 0x100002 | (result.mask << 4), temporary, 0x101e46, 0});
    if (evaluation) {
        if (result.attributeConstantMask)
            initialize.push_back(
                {0x05000036, 0x100002 | (result.attributeConstantMask << 4), temporary, 0x101e46, 0});
        const auto interpolated = result.mask & ~result.attributeConstantMask;
        Row instruction{evaluation->opcode, 0x100002 | (interpolated << 4), temporary, 0x101e46, 0};
        if (evaluation->opcode == 203) {
            instruction.insert(instruction.end(),
                               {0x4002, evaluation->location[0] & 15, evaluation->location[1] & 15, 0, 0});
        } else if (evaluation->opcode == 204) {
            instruction.insert(instruction.end(), {0x4001, evaluation->location[0]});
        }
        instruction[0] |= uint32_t(instruction.size()) << 24;
        if (interpolated)
            initialize.push_back(std::move(instruction));
    }
    for (auto f : templateFields) {
        if (f.reg == UINT32_MAX) {
            fields.push_back(f);
            continue;
        }
        const auto sourceRegister = f.reg;
        if (f.name == "FLORA_VALUE") {
            remap[sourceRegister] = temporary;
            continue;
        }
        if (f.name == "FLORA_FREQUENCY") {
            auto declaration = *originalFrequencyDeclaration;
            const auto index = (declaration[1] & 0x80000000u) ? 3u : 2u;
            const auto mask = (declaration[1] >> 4) & 15;
            if (!mask)
                throw std::runtime_error("Empty original sample-frequency attribute mask");
            const auto component = std::countr_zero(mask);
            const auto original =
                std::find_if(parsed.inputs.begin(), parsed.inputs.end(), [&](const auto &field) {
                    return field.reg == declaration[index] && (field.mask & (1u << component));
                });
            if (original == parsed.inputs.end() || original->type != 3)
                throw std::runtime_error("Invalid original sample-frequency attribute");
            auto field = *original;
            const auto reg = field.reg = nextRegister++;
            field.mask = field.used = 1u << component;
            fields.push_back(field);
            declaration[index] = reg;
            declaration[1] = (declaration[1] & ~0xf0u) | (1u << (component + 4));
            selectedDeclarations.push_back(std::move(declaration));
            minimumPrecision |= index == 3;
            const auto temp = temporary + helperTemps++;
            remap[sourceRegister] = temp;
            initialize.push_back({0x05000036, 0x100012, temp, 0x101006 | ((0x55u * component) << 4), reg});
            continue;
        }
        const bool isPosition = f.system == 1;
        if (!isPosition && f.system != 10 && f.system != 4 && f.system != 9)
            throw std::runtime_error("Unexpected pixel transport input semantic");
        // The compiler can pack multiple system semantics into one register.
        // Preserve that shared temporary while remapping declarations by both
        // register and semantic, including components supplied by the original.
        auto [mapped, inserted] = remap.try_emplace(sourceRegister, temporary + helperTemps);
        if (inserted)
            ++helperTemps;
        const auto temp = mapped->second;
        const auto existing = isPosition ? position : f.system == 10 ? sample : f.system == 4 ? layer : front;
        uint32_t reg = 0, swizzle = 0xe4;
        if (existing) {
            if (!isPosition)
                swizzle = 0x55u * std::countr_zero(existing->mask);
        } else {
            reg = f.reg = nextRegister++;
            if (isPosition && originalPositionDeclaration)
                for (const auto &original : parsed.inputs)
                    if (original.system == 1)
                        f.precision = original.precision;
            fields.push_back(f);
            helperRegisters[{sourceRegister, f.system}] = reg;
        }
        initialize.push_back({0x05000036, 0x100002 | (f.used << 4), temp, 0x101006 | (swizzle << 4), reg});
    }
    std::vector<Row> declarations = selectedDeclarations, executable;
    unsigned conditional = 0;
    for (auto row : program.instructions) {
        const auto op = row[0] & 2047;
        if (op >= 98 && op <= 100 && ((row.at(1) >> 12) & 255) == 1) {
            const auto found = helperRegisters.find({row.at(2), op == 98 ? 0u : row.back()});
            if (found != helperRegisters.end()) {
                if (op == 100 && row.back() == 1 && originalPositionDeclaration) {
                    // Every register pass observes the same original position
                    // interpolation. Only the helper's needed component mask changes.
                    const auto mask = row[1] & 0xf0u;
                    row = *originalPositionDeclaration;
                    row[1] = (row[1] & ~0xf0u) | mask;
                    row[(row[1] & 0x80000000u) ? 3 : 2] = found->second;
                } else
                    row[2] = found->second;
                declarations.push_back(std::move(row));
            }
        } else if (declaration(op)) {
            if (op == 106 && minimumPrecision)
                row[0] |= 1u << 16;
            declarations.push_back(std::move(row));
        } else {
            if (op == 31)
                ++conditional;
            if (op == 21) {
                if (!conditional)
                    throw std::runtime_error("Unbalanced pixel input template branch");
                --conditional;
            }
            if (records && conditional && (op == 11 || op == 12 || (op >= 122 && op <= 125)))
                throw std::runtime_error("Pixel input record derivative became conditional");
            const auto arity = dxbc_detail::operandArities.find(op);
            if (arity == dxbc_detail::operandArities.end() || (row[0] & 0x80000000u))
                throw std::runtime_error("Unexpected pixel transport instruction");
            size_t pos = 1;
            for (unsigned i = 0; i < arity->second; ++i)
                pos = rewrite(row, pos, remap);
            if (pos != row.size())
                throw std::runtime_error("Unexpected pixel transport instruction tail");
            executable.push_back(std::move(row));
        }
    }
    declarations.insert(declarations.end(), initialize.begin(), initialize.end());
    declarations.insert(declarations.end(), executable.begin(), executable.end());
    program.instructions = std::move(declarations);
    const auto inputs = signature(fields), bytes = writeDxbcProgram(program);
    result.bytecode = makeDxbc({{ISG1, inputs}, {OSGN, output}, {SHEX, bytes}});
    for (const auto &f : fields)
        result.inputSignature.push_back(
            {f.name, f.semantic, f.system, f.type, f.reg, f.mask, f.used, f.precision});
    return result;
}
PixelInputShader pixelInputShader(Bytes original, uint32_t inputRegister,
                                  std::optional<PixelInputRecordSelection> records,
                                  std::optional<PixelAttributeEvaluation> evaluation) {
    return buildPixelInputShader(original, inputRegister, records, evaluation);
}
PixelInputShader pixelCoverageShader(Bytes original, const PixelInputRecordSelection &records) {
    return buildPixelInputShader(original, std::nullopt, records);
}
Value PixelInputShader::decode(const std::array<uint32_t, 4> &bits) const { return {bits, uint8_t(mask)}; }
} // namespace flora::dxbc_exec
