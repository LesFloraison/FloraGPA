#include "DxbcExecution.h"
#include "DxbcCheckpointModel.h"
#include "DxbcGpuOperations.h"
#include "DxbcOperandArities.h"
#include "DxbcTextureInputs.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <immintrin.h>
#include <limits>
#include <set>

namespace flora::dxbc_exec {
namespace {
using Words = std::vector<uint32_t>;
constexpr uint32_t absent = UINT32_MAX;
[[noreturn]] void fail(const char *text) { throw std::runtime_error(text); }
struct Operand;
struct Index {
    uint64_t base = 0;
    std::shared_ptr<Operand> relative;
};
struct Operand {
    uint32_t kind = 0, components = 0, mode = 0, mask = 0, modifier = 0, precision = 0;
    std::array<uint32_t, 4> select{0, 1, 2, 3}, literal{};
    std::vector<Index> indices;
};
Operand operand(const Words &words, size_t &pos, unsigned depth = 0) {
    if (depth > 32)
        fail("DXBC execution operand nesting limit");
    const auto token = words.at(pos++);
    Operand result;
    result.kind = (token >> 12) & 255;
    result.components = token & 3;
    result.mode = (token >> 2) & 3;
    if (result.components == 3 || (result.components == 2 && result.mode == 3))
        fail("Unsupported DXBC execution component selection");
    result.mask = result.components == 1 ? 1 : (token >> 4) & 15;
    if (result.components == 1)
        result.select.fill(0);
    else if (result.mode == 1)
        for (unsigned i = 0; i < 4; ++i)
            result.select[i] = (token >> (4 + 2 * i)) & 3;
    else if (result.mode == 2)
        result.select.fill((token >> 4) & 3);
    bool modifier = false;
    for (auto extension = token >> 31; extension;) {
        const auto next = words.at(pos++);
        extension = next >> 31;
        if ((next & 63) != 1 || modifier || (next & 0x7ffe0000u))
            fail("Unsupported DXBC execution operand extension");
        modifier = true;
        result.modifier = (next >> 6) & 255;
        result.precision = (next >> 14) & 7;
        if (result.precision == 3 || result.precision > 5)
            fail("Invalid DXBC minimum precision");
        if (result.modifier > 3)
            fail("Invalid DXBC execution source modifier");
    }
    for (unsigned i = 0; i < ((token >> 20) & 3); ++i) {
        Index index;
        const auto representation = (token >> (22 + i * 3)) & 7;
        if (representation > 4)
            fail("Unsupported DXBC execution index representation");
        if (representation == 0 || representation == 1 || representation == 3 || representation == 4)
            index.base = words.at(pos++);
        if (representation == 1 || representation == 4)
            index.base |= uint64_t(words.at(pos++)) << 32;
        if (representation >= 2)
            index.relative = std::make_shared<Operand>(operand(words, pos, depth + 1));
        if (index.relative && index.relative->precision)
            fail("Minimum-precision relative addressing requires native index conversion");
        result.indices.push_back(std::move(index));
    }
    if (result.kind == 4 || result.kind == 5) {
        if (!result.indices.empty() || (result.components != 1 && result.components != 2) ||
            (result.kind == 5 && result.components != 2))
            fail("Invalid DXBC execution immediate");
        const auto count = result.components == 1 ? 1u : 4u;
        for (unsigned i = 0; i < count; ++i)
            result.literal[i] = words.at(pos++);
        if (count == 1)
            result.literal.fill(result.literal[0]);
        // Immediate payload components are in physical xyzw order, not a mask.
        result.select = {0, 1, 2, 3};
    }
    return result;
}
bool declaration(uint32_t opcode) {
    return (opcode >= 88 && opcode <= 106) || (opcode >= 143 && opcode <= 162) || opcode == 53 ||
           opcode == 206;
}
bool integerSources(uint32_t op) {
    return op == 1 || op == 30 || (op >= 32 && op <= 43) || op == 59 || op == 60 || (op >= 78 && op <= 87) ||
           (op >= 132 && op <= 141);
}
bool precisionArithmetic(uint32_t op) {
    return op <= 1 || (op >= 14 && op <= 17) || (op >= 24 && op <= 30) || (op >= 32 && op <= 43) ||
           op == 47 || (op >= 49 && op <= 57) || op == 59 || op == 60 || (op >= 64 && op <= 68) || op == 75 ||
           (op >= 77 && op <= 87) || (op >= 129 && op <= 142);
}
bool doubleOperation(uint32_t op) {
    return (op >= 191 && op <= 202) || (op >= 210 && op <= 212) || (op >= 214 && op <= 217);
}
bool doubleScalarResult(uint32_t op) {
    return (op >= 195 && op <= 198) || op == 201 || op == 214 || op == 215;
}
bool doubleScalarSource(uint32_t op, size_t source) {
    return op == 202 || op == 216 || op == 217 || (op == 200 && source == 0);
}
uint32_t floatBits(float value) {
    const auto bits = std::bit_cast<uint32_t>(value);
    return (bits & 0x7f800000u) == 0 ? bits & 0x80000000u : bits;
}
float floating(uint32_t bits) {
    if ((bits & 0x7f800000u) == 0)
        bits &= 0x80000000u;
    return std::bit_cast<float>(bits);
}
int32_t signedBits(uint32_t bits) { return std::bit_cast<int32_t>(bits); }
// Do not inherit another caller's rounding mode, exception flags or DAZ/FTZ
// policy. Only the current thread's SSE environment is changed, then restored.
struct FloatEnvironment {
    unsigned original = _mm_getcsr();
    FloatEnvironment() { _mm_setcsr(0x1f80); }
    ~FloatEnvironment() { _mm_setcsr(original); }
};
uint32_t reverseBits(uint32_t v) {
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0f0f0f0fu) << 4) | ((v >> 4) & 0x0f0f0f0fu);
    v = (v << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | (v >> 24);
    return v;
}
uint32_t toHalf(uint32_t bits) {
    const auto sign = (bits >> 16) & 0x8000, fraction = bits & 0x7fffff;
    const auto exponent = int((bits >> 23) & 255);
    if (exponent == 255)
        return sign | (fraction ? 0x7e00u : 0x7c00u);
    const auto halfExponent = exponent - 112;
    // D3D11 float-format conversions truncate toward zero, including finite
    // overflow to the largest finite value (Functional Specification 3.2.2).
    if (halfExponent <= 0)
        return halfExponent < -10 ? sign : sign | ((fraction | 0x800000u) >> unsigned(14 - halfExponent));
    return sign | (halfExponent >= 31 ? 0x7bffu : (uint32_t(halfExponent) << 10) | (fraction >> 13));
}
uint32_t fromHalf(uint32_t bits) {
    const auto sign = (bits & 0x8000u) << 16;
    auto exponent = (bits >> 10) & 31, fraction = bits & 1023;
    if (exponent == 31)
        return sign | 0x7f800000u | (fraction << 13);
    if (exponent)
        return sign | ((exponent + 112) << 23) | (fraction << 13);
    if (!fraction)
        return sign;
    int unbiased = -14;
    while (!(fraction & 1024)) {
        fraction <<= 1;
        --unbiased;
    }
    return sign | (uint32_t(unbiased + 127) << 23) | ((fraction & 1023) << 13);
}
uint32_t scalar(uint32_t op, uint32_t a, uint32_t b, uint32_t c) {
    const auto x = floating(a), y = floating(b), z = floating(c);
    const auto truth = [](bool condition) { return condition ? UINT32_MAX : 0u; };
    switch (op) {
    case 0:
        return floatBits(x + y);
    case 1:
        return a & b;
    case 14:
        return floatBits(x / y);
    case 24:
        return truth(x == y);
    case 25:
        return floatBits(std::exp2(x));
    case 26:
        return floatBits(x - std::floor(x));
    case 27:
        if (std::isnan(x))
            return 0;
        return uint32_t(int32_t(std::clamp(double(x), double(INT32_MIN), double(INT32_MAX))));
    case 28:
        if (std::isnan(x))
            return 0;
        return uint32_t(std::clamp(double(x), 0., double(UINT32_MAX)));
    case 29:
        return truth(x >= y);
    case 30:
        return a + b;
    case 32:
        return truth(a == b);
    case 33:
        return truth(signedBits(a) >= signedBits(b));
    case 34:
        return truth(signedBits(a) < signedBits(b));
    case 35:
    case 82:
        return a * b + c;
    case 36:
        return uint32_t(std::max(signedBits(a), signedBits(b)));
    case 37:
        return uint32_t(std::min(signedBits(a), signedBits(b)));
    case 39:
        return truth(a != b);
    case 40:
        return 0u - a;
    case 41:
        return a << (b & 31);
    case 42:
        return uint32_t(signedBits(a) >> (b & 31));
    case 43:
        return floatBits(float(signedBits(a)));
    case 47:
        return floatBits(std::log2(x));
    case 49:
        return truth(x < y);
    case 50: {
        const volatile float product = floating(floatBits(x * y));
        return floatBits(product + z);
    }
    case 51:
        return floatBits(std::fmin(x, y));
    case 52:
        return floatBits(std::fmax(x, y));
    case 54:
        return a;
    case 56:
        return floatBits(x * y);
    case 57:
        return truth(x != y);
    case 59:
        return ~a;
    case 60:
        return a | b;
    case 64:
        return floatBits(std::nearbyint(x));
    case 65:
        return floatBits(std::floor(x));
    case 66:
        return floatBits(std::ceil(x));
    case 67:
        return floatBits(std::trunc(x));
    case 68:
        return floatBits(1.f / std::sqrt(x));
    case 75:
        return floatBits(std::sqrt(x));
    case 79:
        return truth(a < b);
    case 80:
        return truth(a >= b);
    case 83:
        return std::max(a, b);
    case 84:
        return std::min(a, b);
    case 85:
        return a >> (b & 31);
    case 86:
        return floatBits(float(a));
    case 87:
        return a ^ b;
    case 129:
        return floatBits(1.f / x);
    case 130:
        return toHalf(a);
    case 131:
        return fromHalf(a);
    case 134:
        return std::popcount(a);
    case 135:
        return a ? std::countl_zero(a) : UINT32_MAX;
    case 136:
        return a ? std::countr_zero(a) : UINT32_MAX;
    case 137: {
        const auto v = (a & 0x80000000u) ? ~a : a;
        return v ? std::countl_zero(v) : UINT32_MAX;
    }
    case 138:
    case 139: {
        const auto width = a & 31, offset = b & 31;
        if (!width)
            return 0;
        if (width + offset >= 32)
            return op == 138 ? c >> offset : uint32_t(signedBits(c) >> offset);
        const auto value = (c >> offset) & ((1u << width) - 1);
        return op == 138 || !(value & (1u << (width - 1))) ? value : value | (~0u << width);
    }
    case 141:
        return reverseBits(a);
    default:
        throw std::runtime_error("Unsupported native DXBC scalar opcode " + std::to_string(op));
    }
}
} // namespace
struct ProgramData {
    uint32_t stage = 0, first = 0, temps = 0;
    bool refactoringAllowed = false;
    std::vector<Instruction> instructions;
    std::vector<std::vector<Operand>> operands;
    std::vector<uint32_t> jump, breakTarget, continueTarget;
    std::map<uint32_t, uint32_t> labels;
    struct Interface {
        uint32_t elements, callSites;
        bool dynamic;
        std::set<uint32_t> tables;
    };
    std::set<uint32_t> functionBodies;
    std::map<uint32_t, uint32_t> functionLabels;
    std::map<uint32_t, std::vector<uint32_t>> functionTables;
    std::map<uint32_t, Interface> interfaces;
    uint32_t interfaceSlots = 0;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> arrays;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> cases;
    std::map<uint32_t, uint32_t> defaults;
    std::vector<Value> immediate;
    std::map<Register, uint32_t> bufferStrides;
    std::map<Register, std::pair<uint32_t, uint32_t>> typedResources;
    std::map<Register, uint32_t> samplers;
    std::map<Register, bool> orderedCounters;
    std::map<Register, uint32_t> counterOpcodes;
    std::array<uint32_t, 3> groupSize{};
    std::map<uint32_t, SharedDeclaration> shared;
};
Program::Program(Bytes dxbc) {
    if (dxbc.size() > 64 * 1024 * 1024)
        fail("DXBC execution program budget exceeded");
    auto data = std::make_shared<ProgramData>();
    std::optional<DxbcProgram> code;
    for (const auto &[tag, bytes] : readDxbcParts(dxbc))
        if (tag == 0x52444853 || tag == 0x58454853) {
            if (code)
                fail("Ambiguous DXBC execution program");
            code = readDxbcProgram(bytes);
        }
    if (!code)
        fail("Missing DXBC execution program");
    data->stage = code->header[0] >> 16;
    const auto version = code->header[0] & 0xffff;
    if ((data->stage != 0 && data->stage != 1 && data->stage != 5) ||
        (version != 0x40 && version != 0x41 && version != 0x50))
        fail("Native DXBC execution requires a VS/PS/CS SM4.0, SM4.1 or SM5.0 program");
    uint32_t offset = 2;
    bool executable = false, temps = false;
    uint32_t globalFlags = 0;
    uint64_t arrayElements = 0, sharedBytes = 0;
    for (const auto &words : code->instructions) {
        const auto op = words[0] & 2047;
        if (op == 106) {
            data->refactoringAllowed = (words[0] & (1u << 11)) != 0;
            globalFlags = words[0];
        }
        const bool decl = declaration(op);
        if (decl && executable)
            fail("DXBC declaration follows executable instructions");
        if (!decl)
            executable = true;
        data->instructions.push_back({op, offset, decl, words});
        offset += uint32_t(words.size());
        std::vector<Operand> args;
        if (!decl) {
            const auto found = dxbc_detail::operandArities.find(op);
            if (found == dxbc_detail::operandArities.end())
                fail("Unknown DXBC execution opcode framing");
            size_t pos = 1;
            for (auto extended = words[0] >> 31; extended;)
                extended = words.at(pos++) >> 31;
            if (op == 120)
                ++pos;
            for (unsigned i = 0; i < found->second; ++i)
                args.push_back(operand(words, pos));
            if ((op == 109 && version < 0x41) || ((op == 126 || op == 127 || op == 128) && version != 0x50))
                fail("Gather instruction requires its shader model capability");
            if (doubleOperation(op)) {
                if (version != 0x50 || !(globalFlags & (1u << 12)) ||
                    (op >= 210 && !(globalFlags & (1u << 17))))
                    fail("Double DXBC instruction requires its SM5 capability declarations");
                const auto &destination = args.at(0);
                if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
                    (destination.kind != 0 && destination.kind != 2 && destination.kind != 3) ||
                    (doubleScalarResult(op)
                         ? !destination.mask || std::popcount(destination.mask) > 2
                         : destination.mask != 3 && destination.mask != 12 && destination.mask != 15))
                    fail("Invalid double DXBC destination mask or register");
                for (size_t i = 1; i < args.size(); ++i) {
                    const auto &source = args[i];
                    if (doubleScalarSource(op, i - 1)) {
                        if (source.kind == 5 || (op == 200 && source.modifier))
                            fail("Invalid double DXBC scalar source");
                    } else if (source.components != 2 || source.kind == 4 ||
                               (source.select != std::array<uint32_t, 4>{0, 1, 2, 3} &&
                                source.select != std::array<uint32_t, 4>{0, 1, 0, 1} &&
                                source.select != std::array<uint32_t, 4>{2, 3, 0, 1} &&
                                source.select != std::array<uint32_t, 4>{2, 3, 2, 3}))
                        fail("Invalid double DXBC source pair selection");
                }
            }
            const bool precisionSampling =
                data->stage == 0 && (op == 69 || op == 70 || op == 74) && args[0].kind != 2 &&
                args[2].precision == 0 && args[3].precision == 0 &&
                std::all_of(args.begin(), args.end(), [](const Operand &arg) { return arg.precision <= 2; });
            const bool precisionLoad = [&] {
                if ((op != 45 && !(op == 163 && data->stage == 5)) ||
                    (args[0].kind != 0 && args[0].kind != 3 && args[0].kind != 2) || args[2].precision ||
                    args[2].indices.size() != 1 || args[2].indices[0].relative ||
                    args[2].indices[0].base > UINT32_MAX ||
                    (args[1].precision && args[1].precision != 4 && args[1].precision != 5))
                    return false;
                const auto resource =
                    data->typedResources.find({args[2].kind, {uint32_t(args[2].indices[0].base)}});
                if (resource == data->typedResources.end())
                    return false;
                const auto dimension = resource->second.first;
                if (dimension == 1)
                    return args[0].kind != 2 || !args[0].precision;
                return op == 45 &&
                       (dimension == 2 || dimension == 3 || dimension == 5 || dimension == 7 ||
                        dimension == 8) &&
                       (args[0].kind != 2 || data->stage != 5);
            }();
            const bool precisionStore =
                data->stage == 5 && (op == 166 || op == 168) &&
                (args.back().kind == 0 || args.back().kind == 3) && !args.back().modifier &&
                std::all_of(args.begin(), args.end() - 1, [](const Operand &arg) { return !arg.precision; });
            const bool precisionGather = [&] {
                if (op != 109 && op != 126 && op != 127 && op != 128)
                    return false;
                const bool programmable = op == 127 || op == 128;
                const bool comparison = op == 126 || op == 128;
                return args[1].precision <= 2 && !args[programmable ? 3 : 2].precision &&
                       !args[programmable ? 4 : 3].precision &&
                       (!programmable || !args[2].precision || args[2].precision == 4 ||
                        args[2].precision == 5) &&
                       (!comparison || args.back().precision <= 2);
            }();
            if (!precisionArithmetic(op) && !precisionSampling && !precisionLoad && !precisionStore &&
                !precisionGather &&
                std::any_of(args.begin(), args.end(), [](const Operand &arg) { return arg.precision != 0; }))
                throw std::runtime_error("Minimum precision on DXBC opcode " + std::to_string(op) +
                                         " at word " + std::to_string(data->instructions.back().wordOffset) +
                                         " is not implemented");
            if (pos != words.size() && !dxbc_detail::samplePositionPadding(words, pos))
                fail("DXBC execution instruction operand tail");
        } else if (op == 144) {
            if (version != 0x50 || words.size() != 2 || (words[0] & 0xffffff) != op ||
                !data->functionBodies.insert(words[1]).second)
                fail("Invalid DXBC function body declaration");
        } else if (op == 145) {
            if (version != 0x50 || words.size() < 3 || (words[0] & 0xffffff) != op ||
                words[2] != words.size() - 3 ||
                !data->functionTables.emplace(words[1], Words(words.begin() + 3, words.end())).second)
                fail("Invalid DXBC function table declaration");
        } else if (op == 146) {
            if (version != 0x50 || words.size() < 4 || (words[0] & 0xfff7ff) != op)
                fail("Invalid DXBC interface declaration");
            const auto elements = words[3] >> 16, choices = words[3] & 0xffff;
            if (!elements || words[1] != data->interfaceSlots || elements > 253 - data->interfaceSlots ||
                choices != words.size() - 4 || !choices || words[2] > 4096)
                fail("Invalid DXBC interface array range");
            ProgramData::Interface declaration{elements, words[2], (words[0] & 0x800) != 0, {}};
            for (size_t i = 4; i < words.size(); ++i)
                if (!declaration.tables.insert(words[i]).second)
                    fail("Duplicate DXBC interface function table");
            data->interfaces.emplace(words[1], std::move(declaration));
            data->interfaceSlots += elements;
        } else if (op == 90) {
            size_t pos = 1;
            const auto arg = operand(words, pos);
            const auto mode = (words[0] >> 11) & 15;
            if (pos != words.size() || arg.kind != 6 || arg.components || arg.modifier ||
                arg.indices.size() != 1 || arg.indices[0].relative || arg.indices[0].base >= 16 || mode > 2 ||
                (words[0] & 0x80ff8000) ||
                !data->samplers.emplace(Register{6, {uint32_t(arg.indices[0].base)}}, mode).second)
                fail("Invalid or duplicate DXBC sampler declaration");
        } else if (op == 88 || op == 156) {
            size_t pos = 1;
            const auto arg = operand(words, pos);
            const auto returns = words.at(pos++);
            const auto kind = op == 88 ? 7u : 30u, dimension = (words[0] >> 11) & 31;
            if (pos != words.size() || arg.kind != kind || arg.modifier || arg.indices.size() != 1 ||
                arg.indices[0].relative || arg.indices[0].base >= (kind == 7 ? 128u : 64u))
                fail("Invalid DXBC typed resource declaration");
            if (data->bufferStrides.contains(Register{kind, {uint32_t(arg.indices[0].base)}}) ||
                !data->typedResources
                     .emplace(Register{kind, {uint32_t(arg.indices[0].base)}}, std::pair{dimension, returns})
                     .second)
                fail("Duplicate DXBC typed resource declaration");
        } else if (op == 157 || op == 158 || op == 161 || op == 162) {
            size_t pos = 1;
            const auto arg = operand(words, pos);
            const auto kind = op < 161 ? 30u : 7u;
            const bool structured = op == 158 || op == 162;
            const auto stride = structured ? words.at(pos++) : 0u;
            if (pos != words.size() || arg.kind != kind || arg.indices.size() != 1 ||
                arg.indices[0].relative || arg.indices[0].base >= (kind == 7 ? 128u : 64u) ||
                (structured && (!stride || stride > 2048 || (stride & 3))) ||
                data->typedResources.contains(Register{kind, {uint32_t(arg.indices[0].base)}}) ||
                !data->bufferStrides.emplace(Register{kind, {uint32_t(arg.indices[0].base)}}, stride).second)
                fail("Invalid DXBC buffer declaration");
            if (op == 158)
                data->orderedCounters[{kind, {uint32_t(arg.indices[0].base)}}] = (words[0] & 0x800000) != 0;
        } else if (op == 155) {
            if (data->stage != 5 || data->groupSize[0] || words.size() != 4 || !words[1] || !words[2] ||
                !words[3] || words[1] > 1024 || words[2] > 1024 || words[3] > 64 ||
                uint64_t(words[1]) * words[2] * words[3] > 1024)
                fail("Invalid DXBC thread group dimensions");
            data->groupSize = {words[1], words[2], words[3]};
        } else if (op == 159 || op == 160) {
            size_t pos = 1;
            const auto arg = operand(words, pos);
            const auto stride = op == 160 ? words.at(pos++) : 0u;
            const uint64_t size = uint64_t(words.at(pos++)) * (stride ? stride : 1u);
            if (data->stage != 5 || pos != words.size() || arg.kind != 31 || arg.indices.size() != 1 ||
                arg.indices[0].relative || arg.indices[0].base > UINT32_MAX || !size || (size & 3) ||
                (op == 160 && (!stride || stride > 32768 || (stride & 3))) || size > 32768 - sharedBytes ||
                !data->shared
                     .emplace(uint32_t(arg.indices[0].base), SharedDeclaration{uint32_t(size), stride})
                     .second)
                fail("Invalid DXBC shared memory declaration");
            sharedBytes += size;
            data->bufferStrides.emplace(Register{31, {uint32_t(arg.indices[0].base)}}, stride);
        } else if (op == 104) {
            if (temps || words.size() != 2 || words[1] > 4096)
                fail("Invalid DXBC temporary declaration");
            temps = true;
            data->temps = words[1];
        } else if (op == 105) {
            if (words.size() != 4 || !words[2] || words[3] < 1 || words[3] > 4 ||
                data->arrays.contains(words[1]))
                fail("Invalid DXBC indexable temporary declaration");
            arrayElements += words[2];
            if (arrayElements > 4 * 1024 * 1024)
                fail("DXBC indexable temporary budget exceeded");
            data->arrays[words[1]] = {words[2], words[3]};
        } else if (op == 53 && (words[0] >> 11) == 3) {
            if (!data->immediate.empty() || (words.size() - 2) % 4)
                fail("Invalid DXBC immediate buffer");
            for (size_t i = 2; i < words.size(); i += 4)
                data->immediate.push_back(Value::known({words[i], words[i + 1], words[i + 2], words[i + 3]}));
        }
        data->operands.push_back(std::move(args));
    }
    while (data->first < data->instructions.size() && data->instructions[data->first].declaration)
        ++data->first;
    if (data->first == data->instructions.size())
        fail("DXBC execution program has no executable instructions");
    for (size_t i = data->first; i < data->instructions.size(); ++i) {
        const auto op = data->instructions[i].opcode;
        if (op != 178 && op != 179)
            continue;
        const auto &arg = data->operands[i].at(1);
        if (arg.kind != 30 || arg.modifier || arg.indices.size() != 1 || arg.indices[0].relative ||
            arg.indices[0].base >= 64)
            fail("Invalid DXBC hidden counter operand");
        const Register binding{30, {uint32_t(arg.indices[0].base)}};
        if (!data->orderedCounters.contains(binding))
            fail("Hidden DXBC counter requires a structured UAV declaration");
        const auto [found, inserted] = data->counterOpcodes.emplace(binding, op);
        if (!inserted && found->second != op)
            fail("DXBC shader mixes allocation and consumption on one UAV");
    }
    const auto size = uint32_t(data->instructions.size());
    data->jump.assign(size, absent);
    data->breakTarget.assign(size, absent);
    data->continueTarget.assign(size, absent);
    struct Block {
        uint32_t op, start, alternative = absent;
    };
    std::vector<Block> blocks;
    std::map<uint32_t, uint32_t> owners;
    for (uint32_t i = data->first; i < size; ++i) {
        const auto op = data->instructions[i].opcode;
        for (auto b = blocks.rbegin(); b != blocks.rend(); ++b) {
            if (data->breakTarget[i] == absent && (b->op == 48 || b->op == 76))
                data->breakTarget[i] = b->start;
            if (b->op == 48) {
                data->continueTarget[i] = b->start;
                break;
            }
        }
        if (op == 31 || op == 48 || op == 76)
            blocks.push_back({op, i});
        else if (op == 18) {
            if (blocks.empty() || blocks.back().op != 31 || blocks.back().alternative != absent)
                fail("Unpaired DXBC else");
            blocks.back().alternative = i;
            data->jump[blocks.back().start] = i + 1;
        } else if (op == 21 || op == 22 || op == 23) {
            const auto expected = op == 21 ? 31u : op == 22 ? 48u : 76u;
            if (blocks.empty() || blocks.back().op != expected)
                fail("Unpaired DXBC control terminator");
            const auto b = blocks.back();
            blocks.pop_back();
            if (data->jump[b.start] == absent)
                data->jump[b.start] = i + 1;
            if (b.alternative != absent)
                data->jump[b.alternative] = i + 1;
            owners[b.start] = i;
            if (op == 22)
                data->jump[i] = b.start + 1;
        } else if (op == 6 || op == 10) {
            if (blocks.empty() || blocks.back().op != 76)
                fail("DXBC case outside switch");
            const auto start = blocks.back().start;
            if (op == 10) {
                if (!data->defaults.emplace(start, i + 1).second)
                    fail("Duplicate DXBC default");
            } else {
                const auto &arg = data->operands[i][0];
                if (arg.kind != 4 || arg.components != 1 || arg.modifier ||
                    !data->cases[start].emplace(arg.literal[0], i + 1).second)
                    fail("Invalid DXBC case");
            }
        } else if (op == 44) {
            const auto &arg = data->operands[i][0];
            auto &labels = arg.kind == 17 ? data->functionLabels : data->labels;
            if (!blocks.empty() || (arg.kind != 10 && arg.kind != 17) || arg.components || arg.modifier ||
                arg.precision || arg.indices.size() != 1 || arg.indices[0].relative ||
                arg.indices[0].base > UINT32_MAX ||
                (arg.kind == 17 && !data->functionBodies.contains(uint32_t(arg.indices[0].base))) ||
                !labels.emplace(uint32_t(arg.indices[0].base), i).second)
                fail("Invalid DXBC label");
        }
        if ((op == 2 || op == 3) && data->breakTarget[i] == absent)
            fail("DXBC break outside loop or switch");
        if ((op == 7 || op == 8) && data->continueTarget[i] == absent)
            fail("DXBC continue outside loop");
    }
    if (!blocks.empty())
        fail("Unterminated DXBC control block");
    for (uint32_t i = data->first; i < size; ++i) {
        if (data->breakTarget[i] != absent)
            data->breakTarget[i] = owners.at(data->breakTarget[i]) + 1;
        if (data->continueTarget[i] != absent)
            data->continueTarget[i] = owners.at(data->continueTarget[i]);
    }
    // Include every possible interface target in recursion validation. Execution
    // resolves the selected table and retains a visible return-address stack.
    if (data->interfaces.empty() && std::any_of(data->instructions.begin(), data->instructions.end(),
                                                [](const auto &inst) { return inst.opcode == 120; }))
        fail("DXBC interface call has no interface declaration");
    if (data->functionBodies.empty() && data->functionTables.empty() && data->interfaces.empty())
        checkpoint::subroutines(code->instructions, data->first);
    else {
        for (auto body : data->functionBodies)
            if (!data->functionLabels.contains(body))
                fail("Declared DXBC function body has no label");
        for (const auto &[table, bodies] : data->functionTables)
            for (auto body : bodies)
                if (!data->functionBodies.contains(body))
                    fail("DXBC function table references an absent body");
        for (const auto &[base, declared] : data->interfaces)
            for (auto table : declared.tables)
                if (!data->functionTables.contains(table) ||
                    data->functionTables.at(table).size() != declared.callSites)
                    fail("DXBC interface function table size mismatch");
        constexpr uint64_t main = UINT64_MAX;
        uint64_t owner = main;
        std::map<uint64_t, std::set<uint64_t>> edges{{main, {}}};
        for (uint32_t i = data->first; i < size; ++i) {
            const auto op = data->instructions[i].opcode;
            const auto &args = data->operands[i];
            if (op == 44) {
                if (i == data->first || data->instructions[i - 1].opcode != 62)
                    fail("DXBC function label must follow a return");
                owner = uint64_t(args[0].kind) << 32 | args[0].indices[0].base;
                edges[owner];
            } else if (op == 4 || op == 5) {
                const auto &target = args.back();
                if (target.kind != 10 || target.indices.size() != 1 || target.indices[0].relative ||
                    target.indices[0].base > UINT32_MAX ||
                    !data->labels.contains(uint32_t(target.indices[0].base)))
                    fail("Invalid static DXBC call target");
                edges[owner].insert(uint64_t(10) << 32 | target.indices[0].base);
            } else if (op == 120) {
                const auto &target = args.at(0);
                if (target.kind != 19 || target.components || target.modifier || target.precision ||
                    target.indices.size() != 2 || target.indices[0].relative ||
                    target.indices[0].base > UINT32_MAX ||
                    !data->interfaces.contains(uint32_t(target.indices[0].base)) ||
                    (data->instructions[i].words[0] & 0xffffff) != 120)
                    fail("Invalid DXBC interface call operand");
                const auto &declared = data->interfaces.at(uint32_t(target.indices[0].base));
                const auto callSite = data->instructions[i].words[1];
                if (callSite >= declared.callSites || (target.indices[1].relative && !declared.dynamic) ||
                    (!target.indices[1].relative && target.indices[1].base >= declared.elements))
                    fail("DXBC interface call exceeds its declaration");
                for (auto table : declared.tables)
                    edges[owner].insert(uint64_t(17) << 32 | data->functionTables.at(table).at(callSite));
            }
        }
        std::set<uint64_t> complete, active;
        for (const auto &[root, unused] : edges) {
            if (complete.contains(root))
                continue;
            std::vector<std::pair<uint64_t, bool>> work{{root, false}};
            while (!work.empty()) {
                const auto [node, exit] = work.back();
                work.pop_back();
                if (exit) {
                    active.erase(node);
                    complete.insert(node);
                } else {
                    if (active.contains(node))
                        fail("Recursive DXBC interface calls are invalid");
                    if (complete.contains(node))
                        continue;
                    active.insert(node);
                    work.emplace_back(node, true);
                    for (auto target : edges.at(node))
                        work.emplace_back(target, false);
                }
            }
        }
    }
    data_ = std::move(data);
}
const std::vector<Instruction> &Program::instructions() const { return data_->instructions; }
uint32_t Program::stage() const { return data_->stage; }
std::array<uint32_t, 3> Program::threadGroupSize() const { return data_->groupSize; }
const std::map<uint32_t, SharedDeclaration> &Program::sharedDeclarations() const { return data_->shared; }

struct SamplingRequest {
    Register binding, sampler;
    uint32_t dimension, returns;
    Value address, lodOrDdx;
    std::optional<Value> ddy;
    std::array<uint32_t, 4> swizzle;
    uint32_t mask;
    std::array<int32_t, 3> offset;
    std::array<uint32_t, 3> precision;
};
struct Lane::State {
    std::shared_ptr<const ProgramData> program;
    Inputs inputs;
    std::map<Register, Value> registers;
    std::vector<uint32_t> calls;
    uint32_t pc;
    uint64_t sequence = 0, maxSteps;
    bool done = false, waiting = false;
    std::vector<std::pair<uint32_t, uint64_t>> loops;
    std::vector<size_t> callLoopDepths;
    std::map<uint64_t, std::set<uint32_t>> appendIndices;
    std::optional<std::pair<Value, bool>> pendingSampling;
    std::optional<Value> pendingDerivative;
    bool quad = false, helper = false;
    bool inBounds(const Register &reg) const {
        if (reg.kind == 0)
            return reg.indices.size() == 1 && reg.indices[0] < program->temps;
        if (reg.kind == 3) {
            if (reg.indices.size() != 2)
                return false;
            const auto found = program->arrays.find(reg.indices[0]);
            return found != program->arrays.end() && reg.indices[1] < found->second.first;
        }
        return true;
    }
    Value value(const Register &reg) const {
        if (!inBounds(reg))
            return {};
        if (reg.kind == 8 && reg.indices.size() == 2) {
            const auto cb = inputs.constantBuffers.find(reg.indices[0]);
            if (cb == inputs.constantBuffers.end())
                return {};
            return reg.indices[1] < cb->second.size() ? cb->second[reg.indices[1]] : Value::known({});
        }
        if (reg.kind == 9 && reg.indices.size() == 1)
            return reg.indices[0] < program->immediate.size() ? program->immediate[reg.indices[0]]
                                                              : Value::known({});
        const auto found = registers.find(reg);
        return found == registers.end() ? Value{} : found->second;
    }
    Register address(const Operand &op) const {
        Register result{op.kind, {}};
        for (const auto &index : op.indices) {
            if (index.base > UINT32_MAX)
                fail("DXBC register index exceeds uint32");
            auto value = uint32_t(index.base);
            if (index.relative) {
                const auto relative = source(*index.relative, true);
                if (!(relative.valid & 1))
                    fail("Unknown DXBC relative index");
                value += relative.bits[0];
            }
            result.indices.push_back(value);
        }
        return result;
    }
    Value source(const Operand &op, bool integer) const {
        const auto original = op.kind == 4 || op.kind == 5 ? Value::known(op.literal) : value(address(op));
        Value result;
        for (unsigned i = 0; i < 4; ++i) {
            auto bits = original.bits[op.select[i]];
            if (op.modifier) {
                if (integer) {
                    if (op.modifier & 2)
                        bits = signedBits(bits) < 0 ? 0u - bits : bits;
                    if (op.modifier & 1)
                        bits = 0u - bits;
                } else {
                    if (op.modifier & 2)
                        bits &= 0x7fffffffu;
                    if (op.modifier & 1)
                        bits ^= 0x80000000u;
                }
            }
            result.bits[i] = bits;
            if (original.valid & (1u << op.select[i]))
                result.valid |= uint8_t(1u << i);
        }
        return result;
    }
    std::pair<uint32_t, uint32_t> textureDeclaration(const Operand &resource) const {
        const auto binding = address(resource);
        if (resource.kind == 7 && resource.indices.size() == 1 && resource.indices[0].relative) {
            if (binding.indices[0] >= 128)
                fail("DXBC indexed texture binding exceeds the slot limit");
            // Indexed instructions carry their own type. A class can relocate the
            // resource to a slot with no static declaration or a different type.
            std::optional<uint32_t> dimension, returns;
            const auto &words = program->instructions.at(pc).words;
            for (size_t pos = 1; words.at(pos - 1) >> 31; ++pos) {
                const auto token = words.at(pos), kind = token & 63;
                if (kind == 2) {
                    if (dimension || (token & 0x7ffff800))
                        fail("Invalid indexed texture dimension extension");
                    dimension = (token >> 6) & 31;
                } else if (kind == 3) {
                    if (returns || (token & 0x7fc00000))
                        fail("Invalid indexed texture return extension");
                    returns = (token >> 6) & 0xffff;
                }
            }
            if (!dimension || !returns)
                fail("Indexed texture execution requires instruction type extensions");
            return {*dimension, *returns};
        }
        const auto found = program->typedResources.find(binding);
        if (found == program->typedResources.end())
            fail("Native typed execution requires a resource declaration");
        return found->second;
    }
    void validateSampler(const Operand &sampler, bool comparison) const {
        const auto binding = address(sampler);
        if (binding.indices.size() != 1 || binding.indices[0] >= 16)
            fail("Invalid DXBC sampler binding");
        if (sampler.indices[0].relative)
            return; // The operation selects comparison semantics for indexed samplers.
        const auto found = program->samplers.find(binding);
        if (found == program->samplers.end() || found->second != (comparison ? 1u : 0u))
            fail("Sampling requires a matching sampler declaration");
    }
    SamplingRequest sampling() const {
        const auto &s = *this;
        const auto &p = *program;
        const auto &instruction = p.instructions.at(pc);
        const auto &args = p.operands.at(pc);
        const auto op = instruction.opcode;
        const bool comparison = op == 70 || op == 71;
        const bool implicit = op == 69 || op == 70 || op == 74;
        if (instruction.words[0] & 0x2000)
            fail("Unexpected saturation on a DXBC sampling instruction");
        const auto &destination = args[0], &resource = args[2], &sampler = args[3];
        if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
            (destination.kind != 0 && destination.kind != 2 && destination.kind != 3) || resource.kind != 7 ||
            resource.modifier || sampler.kind != 6 || sampler.components || sampler.modifier ||
            (op != 69 && op != 73 && args[4].components != 1 &&
             !(args[4].components == 2 &&
               (args[4].mode == 2 ||
                (args[4].mode == 1 &&
                 std::all_of(args[4].select.begin(), args[4].select.end(),
                             [&](auto component) { return component == args[4].select[0]; }))))))
            fail("Invalid DXBC explicit sampling operands");
        if (implicit && (p.stage != 0 || resource.indices.size() != 1 || resource.indices[0].relative ||
                         sampler.indices.size() != 1 || sampler.indices[0].relative))
            fail("Implicit sampling requires pixel stage and static texture and sampler bindings");
        if (comparison && std::any_of(resource.select.begin(), resource.select.end(),
                                      [](auto component) { return component != 0; }))
            fail("Comparison sampling requires a replicated red resource selection");
        const auto binding = s.address(resource), samplerBinding = s.address(sampler);
        const auto [dimension, returns] = textureDeclaration(resource);
        validateSampler(sampler, comparison);
        std::array<int32_t, 3> offsets{};
        std::set<uint32_t> extensions;
        for (size_t pos = 1; instruction.words.at(pos - 1) >> 31; ++pos) {
            const auto token = instruction.words.at(pos), kind = token & 63;
            if (!extensions.insert(kind).second)
                fail("Duplicate explicit sampling extension");
            if (kind == 1) {
                if (token & 0x7fe001c0)
                    fail("Invalid explicit sampling offset extension");
                for (unsigned c = 0; c < 3; ++c)
                    offsets[c] = (int32_t((token >> (9 + c * 4)) & 15) ^ 8) - 8;
            } else if (kind == 2) {
                if (((token >> 6) & 31) != dimension || (token & 0x7ffff800))
                    fail("Sampling dimension extension disagrees with its declaration");
            } else if (kind == 3) {
                if (((token >> 6) & 0xffff) != returns || (token & 0x7fc00000))
                    fail("Sampling return extension disagrees with its declaration");
            } else
                fail("Unsupported explicit sampling extension");
        }
        return {binding,
                samplerBinding,
                dimension,
                returns,
                source(args[1], false),
                op == 69 ? Value::known({}) : source(args[4], false),
                op == 73 ? std::optional<Value>(source(args[5], false)) : std::nullopt,
                resource.select,
                destination.mask,
                offsets,
                {destination.precision, args[1].precision, op == 69 ? 0u : args[4].precision}};
    }
};
bool Lane::samplingBoundary() const {
    const auto &s = *state_;
    return !s.done && !s.waiting && !s.pendingSampling && s.program->stage == 5 &&
           s.program->instructions.at(s.pc).opcode == 73 && s.inputs.textures && s.inputs.gpu;
}
bool Lane::hasPendingSampling() const { return state_->pendingSampling.has_value(); }
bool Lane::prepareSamplingBatch(const std::vector<Lane *> &lanes) {
    auto &s = *state_;
    if (!samplingBoundary())
        return false;
    const auto selected = s.sampling();
    std::vector<SamplingInvocation> invocations(lanes.size());
    for (size_t i = 0; i < lanes.size(); ++i) {
        const auto &other = *lanes[i]->state_;
        if (other.done)
            continue;
        if (other.waiting || other.pendingSampling || other.program != s.program || other.pc != s.pc ||
            other.calls != s.calls || other.loops != s.loops || other.callLoopDepths != s.callLoopDepths ||
            other.inputs.textures != s.inputs.textures || other.inputs.gpu != s.inputs.gpu)
            continue;
        const auto request = other.sampling();
        if (request.binding != selected.binding || request.sampler != selected.sampler ||
            request.dimension != selected.dimension || request.returns != selected.returns ||
            request.swizzle != selected.swizzle || request.mask != selected.mask ||
            request.offset != selected.offset)
            continue;
        invocations[i] = {request.address, request.lodOrDdx, *request.ddy, true};
    }
    bool assisted = false;
    const auto values = s.inputs.textures->sampleGradBatch(
        selected.binding, selected.sampler, selected.dimension, selected.returns, s.program->groupSize,
        invocations, selected.swizzle, selected.mask, selected.offset, s.inputs.gpu.get(), assisted);
    if (!values)
        return false;
    // The scheduler drains every staged result before executing any following
    // instruction. Nonparticipating lanes retain their original flat indices.
    for (size_t i = 0; i < lanes.size(); ++i)
        if (invocations[i].active)
            lanes[i]->state_->pendingSampling = std::make_pair((*values)[i], assisted);
    return assisted;
}
Lane::Lane(const Program &program, Inputs inputs, uint64_t maxSteps) {
    if (!maxSteps)
        fail("DXBC execution step budget must be positive");
    uint64_t values = inputs.registers.size();
    for (const auto &[slot, data] : inputs.constantBuffers) {
        if (slot >= 14 || data.size() > 4096)
            fail("DXBC input constant buffer bounds");
        for (const auto &value : data)
            if (value.valid > 15)
                fail("Invalid DXBC constant buffer validity");
        values += data.size();
    }
    if (values > 65536)
        fail("DXBC execution input budget exceeded");
    for (const auto &[reg, value] : inputs.registers) {
        if (reg.kind == 0 || reg.kind == 2 || reg.kind == 3 || reg.kind == 8 || reg.kind == 9 ||
            reg.indices.size() > 3 || value.valid > 15)
            fail("Invalid DXBC execution input register");
    }
    if (inputs.memory) {
        std::map<uint64_t, uint32_t> operations;
        for (const auto &[binding, ordered] : program.data_->orderedCounters) {
            const auto view = inputs.memory->views_.find(binding);
            if (view == inputs.memory->views_.end() || !view->second.resource)
                continue;
            const auto operation = program.data_->counterOpcodes.find(binding);
            if (ordered || operation != program.data_->counterOpcodes.end()) {
                if (!view->second.counter || inputs.memory->counters_.at(*view->second.counter).kind !=
                                                 (ordered ? CounterKind::Ordered : CounterKind::Append))
                    fail("DXBC counter declaration disagrees with the bound view kind");
            } else if (view->second.counter &&
                       inputs.memory->counters_.at(*view->second.counter).kind == CounterKind::Append)
                fail("An APPEND view requires allocation or consumption instructions");
            if (operation != program.data_->counterOpcodes.end()) {
                const auto [found, inserted] = operations.emplace(*view->second.counter, operation->second);
                if (!inserted && found->second != operation->second)
                    fail("DXBC shader mixes allocation and consumption through view aliases");
            }
        }
    }
    state_ = std::make_unique<State>(
        State{program.data_, std::move(inputs), {}, {}, program.data_->first, 0, maxSteps});
    state_->registers = state_->inputs.registers;
    state_->done = state_->pc == state_->program->instructions.size();
}
Lane::~Lane() = default;
bool Lane::finished() const { return state_->done; }
bool Lane::blocked() const { return state_->waiting; }
void Lane::releaseBarrier() { state_->waiting = false; }
Value Lane::value(const Register &reg) const { return state_->value(reg); }
const std::map<Register, Value> &Lane::registers() const { return state_->registers; }
Step Lane::step() {
    auto &s = *state_;
    if (s.done)
        fail("DXBC lane has already finished");
    if (s.waiting)
        fail("DXBC lane is waiting for its thread group");
    if (s.sequence >= s.maxSteps)
        fail("DXBC execution step budget exceeded");
    const auto &p = *s.program;
    const auto &instruction = p.instructions.at(s.pc);
    const auto &args = p.operands.at(s.pc);
    const auto op = instruction.opcode;
    const bool derivative = op == 11 || op == 12 || (op >= 122 && op <= 125);
    Step result{s.sequence, s.pc, instruction.wordOffset, {}, s.calls, {}, false};
    uint32_t next = s.pc + 1;
    auto calls = s.calls;
    auto loops = s.loops;
    auto callLoopDepths = s.callLoopDepths;
    std::optional<std::pair<uint64_t, uint32_t>> appendIndex;
    bool done = false;
    const auto condition = [&](size_t index) {
        const auto value = s.source(args.at(index), true);
        if (!(value.valid & 1))
            fail("Unknown DXBC control condition");
        return (value.bits[0] != 0) == bool(instruction.words[0] & (1u << 18));
    };
    const auto authorizeAppend = [&](const Register &binding, uint32_t index) {
        if (binding.kind != 30 || !s.inputs.memory)
            return;
        const auto view = s.inputs.memory->views_.find(binding);
        if (view == s.inputs.memory->views_.end() || !view->second.counter)
            return;
        const auto id = *view->second.counter;
        if (s.inputs.memory->counters_.at(id).kind == CounterKind::Append) {
            const auto allowed = s.appendIndices.find(id);
            if (allowed == s.appendIndices.end() || !allowed->second.contains(index))
                fail("Undefined APPEND address outside this invocation's allocation or consumption");
        }
    };
    const auto suppressedResource = [&]() -> const Operand * {
        if (!s.quad || !s.helper || op < 163 || op > 189)
            return nullptr;
        unsigned index = 0;
        if (op == 163)
            index = 2;
        else if (op == 165 || op == 167)
            index = unsigned(args.size() - 1);
        else if (op >= 178)
            index = 1;
        return args.at(index).kind == 30 ? &args[index] : nullptr;
    }();
    if (suppressedResource) {
        // Inactive PS invocations cannot access UAV memory or hidden counters.
        // Returned values are undefined, not an observed zero or a speculative read.
        const auto binding = s.address(*suppressedResource);
        const bool typed = p.typedResources.contains(binding);
        const bool buffered = p.bufferStrides.contains(binding);
        const bool declared =
            op == 163 || op == 164 ? typed : buffered || (typed && op >= 169 && op != 178 && op != 179);
        if (suppressedResource->modifier || !declared)
            fail("Invalid suppressed pixel UAV declaration");
        const bool returned = op == 163 || op == 165 || op == 167 || op >= 178;
        result.atomic = op >= 169;
        if (returned && args[0].kind != 13) {
            const auto &destination = args[0];
            if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
                (destination.kind != 0 && destination.kind != 2 && destination.kind != 3) ||
                (result.atomic && std::popcount(destination.mask) != 1))
                fail("Invalid suppressed pixel UAV destination");
            const auto target = s.address(destination);
            if (s.inBounds(target)) {
                const auto before = s.value(target);
                auto after = before;
                for (unsigned c = 0; c < 4; ++c)
                    if (destination.mask & (1u << c)) {
                        if (target.kind == 3 && c >= p.arrays.at(target.indices[0]).second)
                            fail("Suppressed pixel UAV destination exceeds its register components");
                        after.bits[c] = 0;
                        after.valid &= uint8_t(~(1u << c));
                    }
                result.changes.push_back({target, before, after});
            }
        }
    } else if (op == 190) {
        result.syncFlags = instruction.words[0] & 0x7800;
        if (!(result.syncFlags & 0x7000) || (result.syncFlags & 0x6000) == 0x6000 ||
            (p.stage != 5 && result.syncFlags != 0x4000))
            fail("Invalid DXBC synchronization flags for this stage");
        result.syncLoopIterations = loops;
    } else if (op == 48) {
        loops.emplace_back(s.pc, 0);
    } else if (op == 31) {
        if (!condition(0))
            next = p.jump[s.pc];
    } else if (op == 18)
        next = p.jump[s.pc];
    else if (op == 22) {
        if (loops.empty() || p.jump[s.pc] != loops.back().first + 1)
            fail("Invalid DXBC dynamic loop state");
        ++loops.back().second;
        next = p.jump[s.pc];
    } else if (op == 2 || op == 3) {
        if (op == 2 || condition(0)) {
            next = p.breakTarget[s.pc];
            if (!loops.empty() && p.jump[loops.back().first] == next)
                loops.pop_back();
        }
    } else if (op == 7 || op == 8) {
        if (op == 7 || condition(0))
            next = p.continueTarget[s.pc];
    } else if (op == 76) {
        const auto value = s.source(args[0], true);
        if (!(value.valid & 1))
            fail("Unknown DXBC switch condition");
        next = p.defaults.contains(s.pc) ? p.defaults.at(s.pc) : p.jump[s.pc];
        if (p.cases.contains(s.pc)) {
            const auto found = p.cases.at(s.pc).find(value.bits[0]);
            if (found != p.cases.at(s.pc).end())
                next = found->second;
        }
    } else if (op == 120) {
        const auto target = s.address(args.at(0));
        const auto &declared = p.interfaces.at(target.indices[0]);
        if (target.indices[1] >= declared.elements)
            fail("DXBC interface array index is out of range");
        const auto slot = target.indices[0] + target.indices[1];
        const auto binding = s.inputs.interfaceTables.find(slot);
        if (binding == s.inputs.interfaceTables.end())
            fail("DXBC interface slot is unresolved");
        if (!declared.tables.contains(binding->second))
            fail("DXBC interface table is incompatible with its slot");
        if (calls.size() >= 32)
            fail("DXBC call stack budget exceeded");
        calls.push_back(next);
        callLoopDepths.push_back(loops.size());
        const auto body = p.functionTables.at(binding->second).at(instruction.words[1]);
        next = p.functionLabels.at(body);
    } else if (op == 4 || op == 5) {
        if (op == 4 || condition(0)) {
            if (calls.size() >= 32)
                fail("DXBC call stack budget exceeded");
            const auto target = s.address(args.back());
            if (target.kind != 10 || target.indices.size() != 1 || !p.labels.contains(target.indices[0]))
                fail("Invalid DXBC call target");
            calls.push_back(next);
            callLoopDepths.push_back(loops.size());
            next = p.labels.at(target.indices[0]);
        }
    } else if (op == 62 || op == 63) {
        if (op == 62 || condition(0)) {
            if (calls.empty())
                done = true;
            else {
                next = calls.back();
                calls.pop_back();
                loops.resize(callLoopDepths.back());
                callLoopDepths.pop_back();
            }
        }
    } else if (op == 13) {
        if (condition(0)) {
            done = !s.quad;
            result.discarded = true;
        }
    } else if (op == 110 || op == 111) {
        const bool position = op == 110;
        const auto &destination = args[0], &resource = args[1];
        if ((instruction.words[0] & (position ? 0x00fff800u : 0x00fff000u)) || destination.modifier ||
            destination.components != 2 || destination.mode != 0 ||
            (destination.kind != 0 && destination.kind != 2 && destination.kind != 3) || resource.modifier ||
            resource.components != 2 || (resource.mode != 1 && resource.mode != 2) ||
            (resource.kind != 7 && resource.kind != 14) ||
            (resource.kind == 14 ? !resource.indices.empty()
                                 : resource.indices.size() != 1 || resource.indices[0].relative))
            fail("Invalid DXBC sample query operands");
        const auto binding = s.address(resource);
        uint32_t dimension = 0, returns = 0;
        if (resource.kind == 7) {
            const auto declaration = p.typedResources.find(binding);
            if (declaration == p.typedResources.end() ||
                (declaration->second.first != 4 && declaration->second.first != 9))
                fail("Invalid DXBC sample query declaration");
            dimension = declaration->second.first;
            returns = declaration->second.second;
        }
        std::set<uint32_t> extensions;
        for (size_t pos = 1; instruction.words.at(pos - 1) >> 31; ++pos) {
            const auto token = instruction.words.at(pos), kind = token & 63;
            if (!extensions.insert(kind).second || !dimension)
                fail("Invalid DXBC sample query extension");
            if (kind == 2) {
                if (((token >> 6) & 31) != dimension || (token & 0x7ffff800))
                    fail("Sample query dimension extension disagrees with its declaration");
            } else if (kind == 3) {
                if (((token >> 6) & 0xffff) != returns || (token & 0x7fc00000))
                    fail("Sample query return extension disagrees with its declaration");
            } else
                fail("Unsupported DXBC sample query extension");
        }
        Value sample;
        if (position) {
            const auto &index = args[2];
            if (index.modifier || (index.components != 1 && !(index.components == 2 && index.mode == 2)))
                fail("Invalid DXBC sample position index");
            sample = s.source(index, true);
        }
        const auto value = s.inputs.textures
                               ? s.inputs.textures->sampleQuery(binding, dimension, position,
                                                                (instruction.words[0] & 0x800u) != 0, sample,
                                                                result.memoryGpuAssisted)
                               : Value{};
        const auto target = s.address(destination);
        if (s.inBounds(target)) {
            const auto before = s.value(target);
            auto after = before;
            for (unsigned c = 0; c < 4; ++c)
                if ((destination.mask & (1u << c)) &&
                    (target.kind != 3 || c < p.arrays.at(target.indices[0]).second)) {
                    after.bits[c] = value.bits[resource.select[c]];
                    after.valid = uint8_t((after.valid & ~(1u << c)) |
                                          (((value.valid >> resource.select[c]) & 1u) << c));
                }
            result.changes.push_back({target, before, after});
        }
    } else if (op == 61) {
        const auto &destination = args[0], &level = args[1], &resource = args[2];
        const auto mode = (instruction.words[0] >> 11) & 3;
        if ((instruction.words[0] & 0x2000u) || mode > 2 || destination.modifier ||
            destination.components != 2 || destination.mode != 0 ||
            (destination.kind != 0 && destination.kind != 2 && destination.kind != 3) || level.modifier ||
            (level.components != 1 && !(level.components == 2 && level.mode == 2)) || resource.modifier ||
            (resource.kind != 7 && resource.kind != 30) || resource.indices.size() != 1 ||
            resource.indices[0].relative)
            fail("Invalid DXBC resource information operands");
        const auto binding = s.address(resource);
        const auto declaration = p.typedResources.find(binding);
        if (declaration == p.typedResources.end() || declaration->second.first < 2 ||
            declaration->second.first > 10 ||
            (resource.kind == 30 && (declaration->second.first == 4 || declaration->second.first == 6 ||
                                     declaration->second.first >= 9)))
            fail("Invalid DXBC resource information declaration");
        std::set<uint32_t> extensions;
        for (size_t pos = 1; instruction.words.at(pos - 1) >> 31; ++pos) {
            const auto token = instruction.words.at(pos), kind = token & 63;
            if (!extensions.insert(kind).second)
                fail("Duplicate DXBC resource information extension");
            if (kind == 2) {
                if (((token >> 6) & 31) != declaration->second.first || (token & 0x7ffff800))
                    fail("Resource information dimension extension disagrees with its declaration");
            } else if (kind == 3) {
                if (((token >> 6) & 0xffff) != declaration->second.second || (token & 0x7fc00000))
                    fail("Resource information return extension disagrees with its declaration");
            } else
                fail("Unsupported DXBC resource information extension");
        }
        const auto value =
            s.inputs.textures
                ? s.inputs.textures->dimensions(binding, declaration->second.first, s.source(level, true),
                                                mode, s.inputs.gpu.get(), result.memoryGpuAssisted)
                : Value{};
        const auto target = s.address(destination);
        if (s.inBounds(target)) {
            const auto before = s.value(target);
            auto after = before;
            for (unsigned c = 0; c < 4; ++c)
                if ((destination.mask & (1u << c)) &&
                    (target.kind != 3 || c < p.arrays.at(target.indices[0]).second)) {
                    after.bits[c] = value.bits[resource.select[c]];
                    after.valid = uint8_t((after.valid & ~(1u << c)) |
                                          (((value.valid >> resource.select[c]) & 1u) << c));
                }
            result.changes.push_back({target, before, after});
        }
    } else if (op == 109 || op == 126 || op == 127 || op == 128) {
        const bool programmable = op == 127 || op == 128;
        const bool comparison = op == 126 || op == 128;
        const auto &destination = args[0], &resource = args[programmable ? 3 : 2],
                   &sampler = args[programmable ? 4 : 3];
        if ((p.stage != 0 && p.stage != 1 && p.stage != 5) || (instruction.words[0] & 0x00fff800u) ||
            destination.modifier || destination.components != 2 || destination.mode != 0 ||
            (destination.kind != 0 && destination.kind != 2 && destination.kind != 3) || resource.kind != 7 ||
            resource.modifier || resource.components != 2 || (resource.mode != 1 && resource.mode != 2) ||
            sampler.kind != 6 || sampler.modifier || sampler.components != 2 || sampler.mode != 2 ||
            (comparison && sampler.select[0]))
            fail("Invalid DXBC gather operands or stage");
        const auto binding = s.address(resource), samplerBinding = s.address(sampler);
        const auto [dimension, returns] = s.textureDeclaration(resource);
        s.validateSampler(sampler, comparison);
        if ((dimension != 3 && dimension != 6 && dimension != 8 && dimension != 10) ||
            (programmable && dimension != 3 && dimension != 8))
            fail("Invalid DXBC gather texture dimension");
        std::array<int32_t, 3> offsets{};
        std::set<uint32_t> extensions;
        for (size_t pos = 1; instruction.words.at(pos - 1) >> 31; ++pos) {
            const auto token = instruction.words.at(pos), kind = token & 63;
            if (!extensions.insert(kind).second)
                fail("Duplicate DXBC gather extension");
            if (kind == 1) {
                if (token & 0x7fe001c0)
                    fail("Invalid DXBC gather offset extension");
                for (unsigned c = 0; c < 3; ++c)
                    offsets[c] = (int32_t((token >> (9 + c * 4)) & 15) ^ 8) - 8;
            } else if (kind == 2) {
                if (((token >> 6) & 31) != dimension || (token & 0x7ffff800))
                    fail("Gather dimension extension disagrees with declaration");
            } else if (kind == 3) {
                if (((token >> 6) & 0xffff) != returns || (token & 0x7fc00000))
                    fail("Gather return extension disagrees with declaration");
            } else
                fail("Unsupported DXBC gather extension");
        }
        if (offsets[2] || ((programmable || dimension == 6 || dimension == 10) && (offsets[0] || offsets[1])))
            fail("Invalid gather immediate offset for this opcode or dimension");
        GatherOperation operation;
        operation.opcode = op;
        operation.component = sampler.select[0];
        operation.precision[0] = destination.precision;
        operation.refactoringAllowed = p.refactoringAllowed;
        operation.outputDestination = destination.kind == 2;
        const auto gatherSource = [&](const Operand &operand, unsigned index) {
            auto unmodified = operand;
            unmodified.modifier = 0;
            operation.precision[index + 1] = operand.precision;
            operation.modifiers[index] = operand.modifier;
            operation.literal[index] = operand.kind == 4;
            return s.source(unmodified, index == 1);
        };
        const auto address = gatherSource(args[1], 0);
        if (programmable)
            operation.offset = gatherSource(args[2], 1);
        if (comparison) {
            const auto &reference = args.back();
            if (reference.components != 1 &&
                !(reference.components == 2 &&
                  (reference.mode == 2 ||
                   (reference.mode == 1 && std::all_of(reference.select.begin(), reference.select.end(),
                                                       [&](auto c) { return c == reference.select[0]; })))))
                fail("Gather comparison reference requires a scalar selection");
            operation.reference = gatherSource(reference, 2);
        }
        Value value;
        if (s.inputs.textures)
            value = s.inputs.textures->gatherTexture(p.stage, binding, samplerBinding, dimension, returns,
                                                     address, operation, resource.select, destination.mask,
                                                     offsets, s.inputs.gpu.get(), result.memoryGpuAssisted);
        const auto target = s.address(destination);
        if (s.inBounds(target)) {
            const auto before = s.value(target);
            auto after = before;
            for (unsigned c = 0; c < 4; ++c)
                if ((destination.mask & (1u << c)) &&
                    (target.kind != 3 || c < p.arrays.at(target.indices[0]).second)) {
                    after.bits[c] = value.bits[c];
                    after.valid = uint8_t((after.valid & ~(1u << c)) | (value.valid & (1u << c)));
                }
            result.changes.push_back({target, before, after});
        }
    } else if (op >= 69 && op <= 74) {
        if ((op == 69 || op == 70 || op == 74) && (!s.quad || !s.pendingSampling))
            fail("Implicit sampling requires a coherent pixel quad");
        const auto request = s.sampling();
        const auto &destination = args[0];
        const auto target = s.address(destination);
        Value value;
        if (s.pendingSampling) {
            value = s.pendingSampling->first;
            result.memoryGpuAssisted = s.pendingSampling->second;
            s.pendingSampling.reset();
        } else if (s.inputs.textures)
            value = s.inputs.textures->sampleTexture(
                p.stage, request.binding, request.sampler, request.dimension, request.returns,
                request.address, request.lodOrDdx, request.swizzle, request.mask, request.offset,
                s.inputs.gpu.get(), result.memoryGpuAssisted, request.ddy, op == 71);
        if (s.inBounds(target)) {
            const auto before = s.value(target);
            auto after = before;
            for (unsigned c = 0; c < 4; ++c)
                if ((destination.mask & (1u << c)) &&
                    (target.kind != 3 || c < p.arrays.at(target.indices[0]).second)) {
                    after.bits[c] = value.bits[c];
                    after.valid = uint8_t((after.valid & ~(1u << c)) | (value.valid & (1u << c)));
                }
            result.changes.push_back({target, before, after});
        }
    } else if (op == 45 || op == 46 || op == 163 || op == 164) {
        const bool store = op == 164;
        const bool multisample = op == 46;
        const auto &resource = store ? args[0] : args[2];
        if (p.stage != 5 && p.stage != 0 && !(p.stage == 1 && (op == 45 || multisample)))
            fail("Native typed access requires PS/CS execution or a VS SRV load");
        if (instruction.words[0] & (1u << 13))
            fail("Unexpected saturation on a typed buffer instruction");
        if (resource.modifier || resource.kind != (op == 45 || multisample ? 7u : 30u))
            fail("Invalid DXBC typed resource operand");
        const auto binding = s.address(resource);
        const auto declaration = s.textureDeclaration(resource);
        const auto dimension = declaration.first;
        if (multisample != (dimension == 4 || dimension == 9))
            fail("DXBC load opcode disagrees with multisample resource dimension");
        const auto address = s.source(args[1], true);
        if (dimension == 1 && !(address.valid & 1))
            fail("Unknown DXBC typed buffer address");
        if (store) {
            if (resource.components != 2 || resource.mode != 0 || resource.mask != 15 ||
                (dimension == 1 ? !s.inputs.memory : !s.inputs.textures))
                fail("DXBC typed stores require a complete xyzw destination and a snapshot");
            if (dimension == 1)
                result.memoryChanges = s.inputs.memory->storeTyped(
                    binding, address.bits[0], declaration.second, s.source(args[2], false), args[1].kind == 4,
                    result.memoryGpuAssisted, p.stage);
            else {
                std::set<uint32_t> extensions;
                for (size_t pos = 1; instruction.words.at(pos - 1) >> 31; ++pos) {
                    const auto token = instruction.words.at(pos), kind = token & 63;
                    if (!extensions.insert(kind).second ||
                        (kind == 2   ? ((token >> 6) & 31) != dimension || (token & 0x7ffff800)
                         : kind == 3 ? ((token >> 6) & 0xffff) != declaration.second || (token & 0x7fc00000)
                                     : true))
                        fail("Invalid typed texture UAV store extension");
                }
                result.textureChanges = s.inputs.textures->storeUav(
                    binding, dimension, declaration.second, address, s.source(args[2], false),
                    s.inputs.gpu.get(), result.memoryGpuAssisted, p.stage);
            }
        } else {
            const auto &destination = args[0];
            if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
                (destination.kind != 0 && destination.kind != 2 && destination.kind != 3))
                fail("Invalid DXBC typed load destination");
            const auto target = s.address(destination);
            Value value;
            if (dimension == 1)
                value = s.inputs.memory
                            ? s.inputs.memory->loadTyped(
                                  p.stage, binding, address.bits[0], declaration.second, resource.select,
                                  destination.mask, args[1].kind == 4, result.memoryGpuAssisted,
                                  {destination.precision, args[1].precision}, p.refactoringAllowed)
                            : Value{};
            else {
                if (!multisample && dimension != 2 && dimension != 3 && dimension != 5 && dimension != 7 &&
                    dimension != 8)
                    fail("Unsupported native texture load dimension");
                const uint32_t required = (dimension == 2                                       ? 1u
                                           : dimension == 3 || dimension == 4 || dimension == 7 ? 3u
                                                                                                : 7u) |
                                          (op == 45 ? 8u : 0u);
                if ((address.valid & required) != required)
                    fail("Unknown DXBC texture load address");
                std::array<int32_t, 3> offsets{};
                std::set<uint32_t> extensions;
                for (size_t pos = 1; instruction.words.at(pos - 1) >> 31; ++pos) {
                    const auto token = instruction.words.at(pos), kind = token & 63;
                    if (!extensions.insert(kind).second)
                        fail("Duplicate DXBC texture load extension");
                    if (kind == 1 && (op == 45 || multisample)) {
                        for (unsigned c = 0; c < 3; ++c)
                            offsets[c] = (int32_t((token >> (9 + c * 4)) & 15) ^ 8) - 8;
                    } else if (kind == 2) {
                        if (((token >> 6) & 31) != dimension || (token & 0x7ffff800))
                            fail("DXBC texture load dimension extension disagrees with its declaration");
                    } else if (kind == 3) {
                        if (((token >> 6) & 0xffff) != declaration.second)
                            fail("DXBC texture load return extension disagrees with its declaration");
                    } else
                        fail("Unsupported DXBC texture load extension");
                }
                value =
                    !s.inputs.textures ? Value{}
                    : multisample ? s.inputs.textures->loadMultisample(binding, dimension, declaration.second,
                                                                       address, s.source(args[3], true),
                                                                       resource.select, destination.mask,
                                                                       offsets, result.memoryGpuAssisted)
                    : op == 163
                        ? s.inputs.textures->loadUav(binding, dimension, declaration.second, address,
                                                     resource.select, destination.mask, s.inputs.gpu.get(),
                                                     result.memoryGpuAssisted, p.stage)
                        : s.inputs.textures->load(
                              p.stage, binding, dimension, declaration.second, address, resource.select,
                              destination.mask, offsets, s.inputs.gpu.get(), result.memoryGpuAssisted,
                              {destination.precision, args[1].precision}, p.refactoringAllowed,
                              destination.kind == 2, args[1].kind == 4);
            }
            if (s.inBounds(target)) {
                const auto before = s.value(target);
                auto after = before;
                for (unsigned c = 0; c < 4; ++c)
                    if ((destination.mask & (1u << c)) &&
                        (target.kind != 3 || c < p.arrays.at(target.indices[0]).second)) {
                        after.bits[c] = value.bits[c];
                        after.valid = uint8_t((after.valid & ~(1u << c)) | (value.valid & (1u << c)));
                    }
                result.changes.push_back({target, before, after});
            }
        }
    } else if (op >= 165 && op <= 168) {
        const bool store = op == 166 || op == 168, structured = op >= 167;
        if (instruction.words[0] & (1u << 13))
            fail("Unexpected saturation on a DXBC memory instruction");
        const auto &resource = store ? args[0] : args.back();
        if (resource.modifier || (resource.kind != 7 && resource.kind != 30 && resource.kind != 31))
            fail("Unsupported DXBC memory operand");
        const auto memory = resource.kind == 31 ? s.inputs.sharedMemory : s.inputs.memory;
        const auto binding = s.address(resource);
        const auto declaration = p.bufferStrides.find(binding);
        if (declaration == p.bufferStrides.end() || structured != bool(declaration->second))
            fail("DXBC memory instruction disagrees with its declaration");
        if (memory) {
            const auto view = memory->views_.find(binding);
            if (view != memory->views_.end() && view->second.resource &&
                view->second.stride != declaration->second)
                fail("DXBC buffer declaration disagrees with captured stride");
            if (binding.kind == 31 && view != memory->views_.end() && view->second.resource &&
                view->second.size != p.shared.at(binding.indices[0]).bytes)
                fail("DXBC shared memory size disagrees with its declaration");
        }
        const auto indexValue = structured ? s.source(args[1], true) : Value::known({});
        const auto offsetValue = s.source(args[structured ? 2 : 1], true);
        if (!(indexValue.valid & offsetValue.valid & 1))
            fail("Unknown DXBC memory address");
        authorizeAppend(binding, indexValue.bits[0]);
        if (store) {
            if (resource.components != 2 || resource.mode != 0 || !memory)
                fail("Unavailable or invalid DXBC memory store");
            const auto value = s.source(args.back(), false);
            // Register values already contain the observed, widened GPU bits.
            // Raw and structured stores transfer those bits without another narrowing.
            auto &changes = resource.kind == 31 ? result.sharedMemoryChanges : result.memoryChanges;
            changes = memory->store(binding, structured, indexValue.bits[0], offsetValue.bits[0], value,
                                    resource.mask, structured && args[1].kind == 4,
                                    args[structured ? 2 : 1].kind == 4, result.memoryGpuAssisted);
        } else {
            const auto &destination = args[0];
            if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
                (destination.kind != 0 && destination.kind != 2 && destination.kind != 3))
                fail("Invalid DXBC buffer load destination");
            const auto target = s.address(destination);
            const auto value =
                memory ? memory->load(binding, structured, indexValue.bits[0], offsetValue.bits[0],
                                      resource.select, destination.mask, structured && args[1].kind == 4,
                                      args[structured ? 2 : 1].kind == 4, result.memoryGpuAssisted)
                       : Value{};
            if (s.inBounds(target)) {
                const auto before = s.value(target);
                auto after = before;
                for (uint32_t c = 0; c < 4; ++c)
                    if ((destination.mask & (1u << c)) &&
                        (target.kind != 3 || c < p.arrays.at(target.indices[0]).second)) {
                        after.bits[c] = value.bits[c];
                        after.valid = uint8_t((after.valid & ~(1u << c)) | (value.valid & (1u << c)));
                    }
                result.changes.push_back({target, before, after});
            }
        }
    } else if (op == 178 || op == 179) {
        if (!s.inputs.memory)
            fail("Unavailable DXBC hidden counter snapshot");
        const auto binding = s.address(args[1]);
        const auto &destination = args[0];
        if (destination.modifier ||
            (destination.kind != 13 &&
             (destination.components != 2 || destination.mode != 0 || std::popcount(destination.mask) != 1 ||
              (destination.kind != 0 && destination.kind != 2 && destination.kind != 3))))
            fail("Invalid DXBC hidden counter result destination");
        const auto target = s.address(destination);
        const auto view = s.inputs.memory->views_.find(binding);
        if (view != s.inputs.memory->views_.end() && view->second.resource &&
            view->second.stride != p.bufferStrides.at(binding))
            fail("DXBC hidden counter stride disagrees with its declaration");
        const auto [value, change] = s.inputs.memory->counter(binding, op == 179);
        if (change) {
            result.counterChanges.push_back(*change);
            if (change->before.kind == CounterKind::Append && (value.valid & 1)) {
                appendIndex = {{change->view, value.bits[0]}};
                if (op == 178)
                    result.memoryChanges = s.inputs.memory->invalidateAppend(binding, value.bits[0]);
            }
        }
        if (target.kind != 13 && s.inBounds(target)) {
            const auto component = uint32_t(std::countr_zero(destination.mask));
            if (target.kind == 3 && component >= p.arrays.at(target.indices[0]).second)
                fail("DXBC hidden counter destination exceeds its register components");
            const auto before = s.value(target);
            auto after = before;
            after.bits[component] = value.bits[0];
            after.valid = uint8_t((after.valid & ~destination.mask) | ((value.valid & 1) << component));
            result.changes.push_back({target, before, after});
        }
        result.atomic = true;
    } else if ((op >= 169 && op <= 177) || (op >= 180 && op <= 189)) {
        const bool returned = op >= 180, compare = op == 172 || op == 185;
        const size_t first = returned ? 1 : 0;
        const auto &resource = args[first];
        if (resource.modifier || (resource.kind != 30 && resource.kind != 31))
            fail("Invalid DXBC atomic resource");
        const auto binding = s.address(resource);
        const auto address = s.source(args[first + 1], true);
        const auto value = s.source(args.back(), true);
        const auto comparison = compare ? s.source(args[first + 2], true) : Value{};
        Value old;
        if (const auto typed = p.typedResources.find(binding); typed != p.typedResources.end()) {
            if (resource.kind != 30)
                fail("Invalid DXBC typed atomic resource");
            const auto [dimension, returns] = typed->second;
            std::pair<Value, std::vector<MemoryChange>> transaction;
            if (dimension == 1) {
                if (!s.inputs.memory || !(address.valid & 1))
                    fail("Unavailable typed atomic buffer or address");
                transaction =
                    s.inputs.memory->atomicTyped(binding, op, address.bits[0], returns, value, comparison);
                result.memoryChanges = std::move(transaction.second);
            } else {
                if (!s.inputs.textures)
                    fail("Unavailable typed atomic texture inputs");
                transaction =
                    s.inputs.textures->atomicUav(binding, dimension, returns, address, op, value, comparison,
                                                 s.inputs.gpu.get(), result.memoryGpuAssisted);
                result.textureChanges = std::move(transaction.second);
            }
            old = transaction.first;
        } else {
            const auto declaration = p.bufferStrides.find(binding);
            if (declaration == p.bufferStrides.end())
                fail("Missing DXBC atomic buffer declaration");
            const auto memory = resource.kind == 31 ? s.inputs.sharedMemory : s.inputs.memory;
            if (!memory)
                fail("Unavailable DXBC atomic memory");
            const auto view = memory->views_.find(binding);
            if (view != memory->views_.end() && view->second.resource &&
                view->second.stride != declaration->second)
                fail("DXBC atomic declaration disagrees with captured stride");
            if (binding.kind == 31 && view != memory->views_.end() && view->second.resource &&
                view->second.size != p.shared.at(binding.indices[0]).bytes)
                fail("DXBC atomic shared memory size disagrees with its declaration");
            const bool structured = declaration->second != 0;
            if ((address.valid & (structured ? 3 : 1)) != (structured ? 3 : 1))
                fail("Unknown DXBC atomic address");
            authorizeAppend(binding, structured ? address.bits[0] : 0);
            const auto transaction = memory->atomic(binding, op, structured ? address.bits[0] : 0,
                                                    address.bits[structured ? 1 : 0], value, comparison);
            old = transaction.first;
            (resource.kind == 31 ? result.sharedMemoryChanges : result.memoryChanges) = transaction.second;
        }
        if (returned && args[0].kind != 13) {
            const auto &destination = args[0];
            if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
                std::popcount(destination.mask) != 1 ||
                (destination.kind != 0 && destination.kind != 2 && destination.kind != 3))
                fail("Invalid DXBC atomic result destination");
            const auto target = s.address(destination);
            if (s.inBounds(target)) {
                const auto component = uint32_t(std::countr_zero(destination.mask));
                if (target.kind == 3 && component >= p.arrays.at(target.indices[0]).second)
                    fail("DXBC atomic destination exceeds its register components");
                const auto before = s.value(target);
                auto after = before;
                after.bits[component] = old.bits[0];
                after.valid = uint8_t((after.valid & ~destination.mask) | ((old.valid & 1) << component));
                result.changes.push_back({target, before, after});
            }
        }
        result.atomic = true;
    } else if (doubleOperation(op)) {
        if (!s.inputs.gpu)
            fail("Double DXBC execution requires the selected adapter GPU assist");
        const auto &destination = args[0];
        const auto target = s.address(destination);
        if (target.kind == 3) {
            const auto array = target.indices.size() == 2 ? p.arrays.find(target.indices[0]) : p.arrays.end();
            if (array == p.arrays.end() || destination.mask >= (1u << array->second.second))
                fail("Double DXBC destination exceeds indexable register components");
        }
        std::vector<Value> sources;
        for (size_t i = 1; i < args.size(); ++i) {
            if (doubleScalarSource(op, i - 1))
                sources.push_back(s.source(args[i], op != 202));
            else {
                auto raw = args[i];
                raw.modifier = 0;
                auto value = s.source(raw, true);
                for (unsigned high : {1u, 3u}) {
                    if (args[i].modifier & 2)
                        value.bits[high] &= 0x7fffffffu;
                    if (args[i].modifier & 1)
                        value.bits[high] ^= 0x80000000u;
                }
                sources.push_back(value);
            }
        }
        const auto before = s.value(target);
        const auto native = s.inputs.gpu
                                ->instruction(p.stage, instruction, {target}, {destination.mask}, sources,
                                              {before}, p.refactoringAllowed)
                                .at(0);
        auto after = before;
        unsigned scalar = 0;
        for (unsigned component = 0; component < 4; ++component) {
            if (!(destination.mask & (1u << component)))
                continue;
            const unsigned pair = doubleScalarResult(op) ? scalar++ : component / 2;
            bool known = true;
            for (size_t i = 0; i < sources.size(); ++i) {
                if (op == 200 && i != 0 && i != (sources[0].bits[pair] ? 1u : 2u))
                    continue;
                const uint8_t required = uint8_t(doubleScalarSource(op, i) ? 1u << pair : 3u << (2 * pair));
                known &= (sources[i].valid & required) == required;
            }
            after.bits[component] = native.bits[component];
            after.valid = uint8_t((after.valid & ~(1u << component)) | (known ? 1u << component : 0u));
        }
        if (s.inBounds(target))
            result.changes.push_back({target, before, after});
        result.instructionGpuAssisted = true;
    } else if (op == 6 || op == 10 || op == 21 || op == 23 || op == 44 || op == 48 || op == 58) {
        // Structural instructions remain visible in the trace.
    } else {
        const bool dual = op == 38 || op == 77 || op == 78 || op == 81 || op == 132 || op == 133 || op == 142;
        const size_t destinations = dual ? 2 : 1;
        if (args.size() <= destinations)
            throw std::runtime_error("Unsupported native DXBC opcode " + std::to_string(op));
        std::vector<Register> targets;
        for (size_t i = 0; i < destinations; ++i) {
            const auto &arg = args[i];
            if (arg.modifier || (arg.kind != 13 && arg.components == 2 && arg.mode != 0))
                fail("Invalid DXBC execution destination");
            if (arg.kind != 0 && arg.kind != 2 && arg.kind != 3 && arg.kind != 12 && arg.kind != 13 &&
                arg.kind != 15 && arg.kind != 38 && arg.kind != 39)
                fail("Unsupported DXBC execution destination kind");
            targets.push_back(s.address(arg));
        }
        if (op == 142 && (targets[0] == targets[1] || !args[0].mask || !args[1].mask))
            fail("DXBC swapc requires distinct destinations with nonempty masks");
        if (dual && targets[0] == targets[1] && targets[0].kind != 13 && (args[0].mask & args[1].mask))
            fail("Overlapping DXBC multi-result destinations");
        std::vector<Value> values;
        for (size_t i = destinations; i < args.size(); ++i)
            values.push_back(s.source(args[i], integerSources(op)));
        std::array<Value, 2> outputs{};
        FloatEnvironment environment;
        if (derivative) {
            if (!s.quad || !s.pendingDerivative || p.stage != 0 || values.size() != 1)
                fail("DXBC derivatives require a coherent pixel quad");
            outputs[0] = *s.pendingDerivative;
            result.instructionGpuAssisted = true;
        } else if (op >= 15 && op <= 17) {
            const auto count = op - 13;
            float sum = 0;
            bool valid = true;
            for (unsigned i = 0; i < count; ++i) {
                const auto product =
                    floating(floatBits(floating(values[0].bits[i]) * floating(values[1].bits[i])));
                sum = i ? floating(floatBits(sum + product)) : product;
                valid &= (values[0].valid & values[1].valid & (1u << i)) != 0;
            }
            outputs[0] = {{floatBits(sum), floatBits(sum), floatBits(sum), floatBits(sum)},
                          uint8_t(valid ? 15 : 0)};
        } else
            for (unsigned i = 0; i < 4; ++i) {
                bool valid = true;
                for (const auto &value : values)
                    valid &= (value.valid & (1u << i)) != 0;
                const auto a = values[0].bits[i], b = values.size() > 1 ? values[1].bits[i] : 0u,
                           c = values.size() > 2 ? values[2].bits[i] : 0u;
                if (op == 55 || op == 142) {
                    // An unknown unselected branch does not taint a known selection.
                    const auto selected = (bool(a) != (op == 142)) ? 1u : 2u;
                    valid = (values[0].valid & values[selected].valid & (1u << i)) != 0;
                    outputs[0].bits[i] = values[selected].bits[i];
                    if (op == 142) {
                        const auto other = 3u - selected;
                        outputs[1].bits[i] = values[other].bits[i];
                        if (values[0].valid & values[other].valid & (1u << i))
                            outputs[1].valid |= uint8_t(1u << i);
                    }
                } else if (op == 38 || op == 81) {
                    const auto product = op == 38 ? uint64_t(int64_t(signedBits(a)) * int64_t(signedBits(b)))
                                                  : uint64_t(a) * b;
                    outputs[0].bits[i] = uint32_t(product >> 32);
                    outputs[1].bits[i] = uint32_t(product);
                } else if (op == 78) {
                    outputs[0].bits[i] = b ? a / b : UINT32_MAX;
                    outputs[1].bits[i] = b ? a % b : UINT32_MAX;
                } else if (op == 77) {
                    outputs[0].bits[i] = floatBits(std::sin(floating(a)));
                    outputs[1].bits[i] = floatBits(std::cos(floating(a)));
                } else if (op == 132 || op == 133) {
                    outputs[0].bits[i] = op == 132 ? a + b : a - b;
                    outputs[1].bits[i] = op == 132 ? uint32_t((uint64_t(a) + b) >> 32) : uint32_t(a < b);
                } else if (op == 140) {
                    const auto width = a & 31, offset = b & 31;
                    const auto mask = width ? ((1u << width) - 1) << offset : 0u;
                    outputs[0].bits[i] = (c << offset & mask) | (values.at(3).bits[i] & ~mask);
                } else
                    outputs[0].bits[i] = scalar(op, a, b, c);
                if (valid) {
                    outputs[0].valid |= uint8_t(1u << i);
                    if (dual && op != 142)
                        outputs[1].valid |= uint8_t(1u << i);
                }
            }
        const bool aliased = dual && targets[0] == targets[1] && targets[0].kind != 13;
        const bool literalConversion = (op == 27 || op == 28 || op == 131) && args[destinations].kind == 4;
        const bool literalBorrow = op == 133 && args[2].kind == 4 && args[3].kind == 4;
        const bool minimumPrecision =
            std::any_of(args.begin(), args.end(), [](const Operand &arg) { return arg.precision != 0; });
        if (minimumPrecision && !s.inputs.gpu)
            fail("Minimum-precision arithmetic requires the selected adapter GPU assist");
        const bool nativeFloat = p.stage == 1 && (op == 50 || (op >= 15 && op <= 17));
        if (s.inputs.gpu &&
            (aliased || literalConversion || literalBorrow || minimumPrecision || nativeFloat)) {
            std::vector<uint32_t> masks;
            std::vector<Value> before;
            for (size_t i = 0; i < destinations; ++i) {
                masks.push_back(args[i].mask);
                before.push_back(s.value(targets[i]));
            }
            const auto native = s.inputs.gpu->instruction(p.stage, instruction, targets, masks, values,
                                                          before, p.refactoringAllowed);
            const bool known =
                std::all_of(values.begin(), values.end(), [](const Value &v) { return v.valid == 15; });
            for (size_t i = 0; i < destinations; ++i) {
                outputs[i].bits = native.at(i).bits;
                // A narrowed condition can select a different branch. Do not use
                // the CPU's unrounded selection to validate an unknown result.
                if ((op == 55 || op == 142) && args[destinations].precision)
                    outputs[i].valid = values[0].valid & values[1].valid & values[2].valid;
                if (!known && !minimumPrecision && !nativeFloat)
                    outputs[i].valid = 0;
            }
            result.instructionGpuAssisted = true;
        }
        for (size_t i = 0; i < destinations; ++i) {
            const auto &target = targets[i];
            if (target.kind == 13 || !s.inBounds(target))
                continue;
            const auto previous = std::find_if(result.changes.begin(), result.changes.end(),
                                               [&](const Change &c) { return c.destination == target; });
            auto after = previous == result.changes.end() ? s.value(target) : previous->after;
            const auto before = after;
            for (unsigned c = 0; c < 4; ++c)
                if (args[i].mask & (1u << c)) {
                    if (target.kind == 3 && c >= p.arrays.at(target.indices[0]).second)
                        continue;
                    auto bits = outputs[i].bits[c];
                    if ((!result.instructionGpuAssisted || derivative) &&
                        (instruction.words[0] & (1u << 13))) {
                        const auto number = floating(bits);
                        bits = floatBits(std::isnan(number) ? 0.f : std::clamp(number, 0.f, 1.f));
                    }
                    after.bits[c] = bits;
                    after.valid = uint8_t((after.valid & ~(1u << c)) | (outputs[i].valid & (1u << c)));
                }
            if (previous == result.changes.end())
                result.changes.push_back({target, before, after});
            else
                previous->after = after;
        }
    }
    if (!done && next >= p.instructions.size())
        fail("DXBC execution fell off the program without ret");
    // Commit only after the complete instruction has validated and computed.
    if (appendIndex)
        s.appendIndices[appendIndex->first].insert(appendIndex->second);
    for (const auto &change : result.changes)
        s.registers[change.destination] = change.after;
    if (!result.memoryChanges.empty())
        s.inputs.memory->commit(result.memoryChanges);
    if (!result.sharedMemoryChanges.empty())
        s.inputs.sharedMemory->commit(result.sharedMemoryChanges);
    if (!result.counterChanges.empty())
        s.inputs.memory->commitCounters(result.counterChanges);
    if (!result.textureChanges.empty())
        s.inputs.textures->apply(result.textureChanges);
    s.calls = std::move(calls);
    s.loops = std::move(loops);
    s.callLoopDepths = std::move(callLoopDepths);
    s.pc = next;
    if (derivative)
        s.pendingDerivative.reset();
    if (s.quad && result.discarded)
        s.helper = true;
    s.done = done;
    s.waiting = (result.syncFlags & 0x800) != 0;
    ++s.sequence;
    if (!done)
        result.nextInstruction = next;
    return result;
}
struct PixelQuad::State {
    std::array<std::unique_ptr<Lane>, 4> lanes;
    CancelCheck cancelled;
    uint64_t sequence = 0, maxSteps, batches = 0, sampling = 0;
    uint32_t cursor = 0, waitingSteps = 0;
    uint8_t active;
    bool failed = false;
};
PixelQuad::PixelQuad(const Program &program, std::array<Inputs, 4> inputs, uint8_t coverage,
                     uint64_t maxSteps, CancelCheck cancelled)
    : state_(std::make_unique<State>()) {
    if (program.stage() != 0 || coverage > 15 || !maxSteps)
        fail("Invalid DXBC pixel quad configuration");
    auto &s = *state_;
    s.maxSteps = maxSteps;
    s.active = coverage;
    s.cancelled = std::move(cancelled);
    for (unsigned i = 1; i < 4; ++i)
        if (inputs[i].gpu != inputs[0].gpu || inputs[i].memory != inputs[0].memory ||
            inputs[i].textures != inputs[0].textures || inputs[i].sharedMemory)
            fail("Pixel quad lanes require common private resources and GPU identity");
    if (inputs[0].sharedMemory)
        fail("Pixel quad cannot use compute shared memory");
    for (unsigned i = 0; i < 4; ++i) {
        checkCancellation(s.cancelled);
        s.lanes[i] = std::make_unique<Lane>(program, std::move(inputs[i]), maxSteps);
        s.lanes[i]->state_->quad = true;
        s.lanes[i]->state_->helper = !(coverage & (1u << i));
    }
}
PixelQuad::~PixelQuad() = default;
bool PixelQuad::finished() const {
    return !state_->failed && std::all_of(state_->lanes.begin(), state_->lanes.end(),
                                          [](const auto &lane) { return lane->finished(); });
}
const Lane &PixelQuad::lane(uint32_t index) const { return *state_->lanes.at(index); }
uint8_t PixelQuad::activeMask() const { return state_->active; }
uint64_t PixelQuad::derivativeBatches() const { return state_->batches; }
uint64_t PixelQuad::samplingBatches() const { return state_->sampling; }
PixelQuadStep PixelQuad::step() {
    auto &s = *state_;
    if (s.failed)
        fail("Recreate the DXBC pixel quad after a failed step");
    try {
        checkCancellation(s.cancelled);
        if (s.sequence >= s.maxSteps)
            fail("DXBC pixel quad step budget exceeded");
        uint32_t ordinary = 4, boundary = 4, pending = 4;
        for (unsigned n = 0; n < 4; ++n) {
            const auto i = (s.cursor + n) % 4;
            const auto &lane = *s.lanes[i]->state_;
            if (lane.done)
                continue;
            if (lane.waiting)
                fail("Pixel quad encountered a compute barrier");
            const auto op = lane.program->instructions.at(lane.pc).opcode;
            if (lane.pendingDerivative || lane.pendingSampling) {
                if (pending == 4)
                    pending = i;
            } else if (op == 11 || op == 12 || (op >= 122 && op <= 125) || op == 69 || op == 70 || op == 74) {
                if (boundary == 4)
                    boundary = i;
            } else if (ordinary == 4)
                ordinary = i;
        }
        auto selected = pending != 4 ? pending : ordinary;
        if (pending == 4 && boundary != 4) {
            if (ordinary != 4) {
                if (++s.waitingSteps > 4096)
                    fail("Pixel quad rendezvous exceeded its progress budget");
            } else {
                const auto &first = *s.lanes[boundary]->state_;
                for (unsigned i = 0; i < 4; ++i) {
                    const auto &other = *s.lanes[i]->state_;
                    if (other.done || other.pc != first.pc || other.calls != first.calls ||
                        other.loops != first.loops || other.callLoopDepths != first.callLoopDepths)
                        fail("Pixel lanes did not reach the same dynamic quad instruction");
                }
                const auto opcode = first.program->instructions.at(first.pc).opcode;
                if (opcode == 69 || opcode == 70 || opcode == 74) {
                    const auto request = first.sampling();
                    std::array<PixelSamplingInvocation, 4> invocations;
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto item = s.lanes[i]->state_->sampling();
                        if (item.binding != request.binding || item.sampler != request.sampler ||
                            item.dimension != request.dimension || item.returns != request.returns ||
                            item.swizzle != request.swizzle || item.mask != request.mask ||
                            item.offset != request.offset || item.precision != request.precision)
                            fail("Pixel sampling cohort bindings disagree");
                        invocations[i] = {item.address, item.lodOrDdx};
                    }
                    bool assisted = false;
                    const auto results =
                        first.inputs.textures
                            ? first.inputs.textures->samplePixelQuad(
                                  opcode, request.binding, request.sampler, request.dimension,
                                  request.returns, invocations, request.swizzle, request.mask, request.offset,
                                  first.inputs.gpu.get(), assisted, request.precision)
                            : std::array<Value, 4>{};
                    for (unsigned i = 0; i < 4; ++i)
                        s.lanes[i]->state_->pendingSampling = std::make_pair(results[i], assisted);
                    ++s.sampling;
                } else {
                    if (!first.inputs.gpu)
                        fail("Pixel derivatives require the selected adapter GPU assist");
                    std::array<Value, 4> values;
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto &lane = *s.lanes[i]->state_;
                        values[i] = lane.source(lane.program->operands.at(lane.pc).at(1), false);
                    }
                    const auto results = first.inputs.gpu->pixelDerivatives(opcode, values);
                    for (unsigned i = 0; i < 4; ++i)
                        s.lanes[i]->state_->pendingDerivative = results[i];
                    ++s.batches;
                }
                selected = boundary;
                s.waitingSteps = 0;
            }
        } else if (pending == 4)
            s.waitingSteps = 0;
        if (selected == 4)
            fail("DXBC pixel quad has no executable lane");
        const bool helper = s.lanes[selected]->state_->helper;
        auto result = s.lanes[selected]->step();
        if (result.discarded)
            s.active &= uint8_t(~(1u << selected));
        PixelQuadStep output{s.sequence++, selected, helper, s.active, std::move(result)};
        s.cursor = (selected + 1) % 4;
        return output;
    } catch (...) {
        s.failed = true;
        throw;
    }
}
} // namespace flora::dxbc_exec
