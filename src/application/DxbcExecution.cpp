#include "DxbcExecution.h"
#include "DxbcCheckpointModel.h"
#include "DxbcOperandArities.h"
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
    uint32_t kind = 0, components = 0, mode = 0, mask = 0, modifier = 0;
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
        if ((next & 63) != 1 || modifier || (next & 0x7fffc000u))
            fail("Unsupported DXBC execution operand extension");
        modifier = true;
        result.modifier = (next >> 6) & 255;
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
    std::vector<Instruction> instructions;
    std::vector<std::vector<Operand>> operands;
    std::vector<uint32_t> jump, breakTarget, continueTarget;
    std::map<uint32_t, uint32_t> labels;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> arrays;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> cases;
    std::map<uint32_t, uint32_t> defaults;
    std::vector<Value> immediate;
    std::map<Register, uint32_t> bufferStrides;
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
    uint64_t arrayElements = 0;
    for (const auto &words : code->instructions) {
        const auto op = words[0] & 2047;
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
            if (pos != words.size())
                fail("DXBC execution instruction operand tail");
        } else if (op == 157 || op == 158 || op == 161 || op == 162) {
            size_t pos = 1;
            const auto arg = operand(words, pos);
            const auto kind = op < 161 ? 30u : 7u;
            const bool structured = op == 158 || op == 162;
            const auto stride = structured ? words.at(pos++) : 0u;
            if (pos != words.size() || arg.kind != kind || arg.indices.size() != 1 ||
                arg.indices[0].relative || arg.indices[0].base >= (kind == 7 ? 128u : 64u) ||
                (structured && (!stride || stride > 2048 || (stride & 3))) ||
                !data->bufferStrides.emplace(Register{kind, {uint32_t(arg.indices[0].base)}}, stride).second)
                fail("Invalid DXBC buffer declaration");
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
            if (!blocks.empty() || arg.kind != 10 || arg.indices.size() != 1 || arg.indices[0].relative ||
                arg.indices[0].base > UINT32_MAX ||
                !data->labels.emplace(uint32_t(arg.indices[0].base), i).second)
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
    // Validate static labels and reject recursion using the existing project-owned
    // call graph. Execution retains its own visible return-address stack.
    checkpoint::subroutines(code->instructions, data->first);
    data_ = std::move(data);
}
const std::vector<Instruction> &Program::instructions() const { return data_->instructions; }
uint32_t Program::stage() const { return data_->stage; }

struct Lane::State {
    std::shared_ptr<const ProgramData> program;
    Inputs inputs;
    std::map<Register, Value> registers;
    std::vector<uint32_t> calls;
    uint32_t pc;
    uint64_t sequence = 0, maxSteps;
    bool done = false;
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
};
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
    state_ = std::make_unique<State>(
        State{program.data_, std::move(inputs), {}, {}, program.data_->first, 0, maxSteps});
    state_->registers = state_->inputs.registers;
    state_->done = state_->pc == state_->program->instructions.size();
}
Lane::~Lane() = default;
bool Lane::finished() const { return state_->done; }
Value Lane::value(const Register &reg) const { return state_->value(reg); }
const std::map<Register, Value> &Lane::registers() const { return state_->registers; }
Step Lane::step() {
    auto &s = *state_;
    if (s.done)
        fail("DXBC lane has already finished");
    if (s.sequence >= s.maxSteps)
        fail("DXBC execution step budget exceeded");
    const auto &p = *s.program;
    const auto &instruction = p.instructions.at(s.pc);
    const auto &args = p.operands.at(s.pc);
    const auto op = instruction.opcode;
    Step result{s.sequence, s.pc, instruction.wordOffset, {}, s.calls, {}, false};
    uint32_t next = s.pc + 1;
    auto calls = s.calls;
    bool done = false;
    const auto condition = [&](size_t index) {
        const auto value = s.source(args.at(index), true);
        if (!(value.valid & 1))
            fail("Unknown DXBC control condition");
        return (value.bits[0] != 0) == bool(instruction.words[0] & (1u << 18));
    };
    if (op == 31) {
        if (!condition(0))
            next = p.jump[s.pc];
    } else if (op == 18 || op == 22)
        next = p.jump[s.pc];
    else if (op == 2 || op == 3) {
        if (op == 2 || condition(0))
            next = p.breakTarget[s.pc];
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
    } else if (op == 4 || op == 5) {
        if ((op == 4 || condition(0)) && calls.size() < 32) {
            const auto target = s.address(args.back());
            if (target.kind != 10 || target.indices.size() != 1 || !p.labels.contains(target.indices[0]))
                fail("Invalid DXBC call target");
            calls.push_back(next);
            next = p.labels.at(target.indices[0]);
        }
    } else if (op == 62 || op == 63) {
        if (op == 62 || condition(0)) {
            if (calls.empty())
                done = true;
            else {
                next = calls.back();
                calls.pop_back();
            }
        }
    } else if (op == 13) {
        if (condition(0)) {
            done = true;
            result.discarded = true;
        }
    } else if (op >= 165 && op <= 168) {
        const bool store = op == 166 || op == 168, structured = op >= 167;
        if (instruction.words[0] & (1u << 13))
            fail("Unexpected saturation on a DXBC memory instruction");
        const auto &resource = store ? args[0] : args.back();
        if (resource.modifier || (resource.kind != 7 && resource.kind != 30))
            fail("Unsupported DXBC memory operand");
        const auto binding = s.address(resource);
        const auto declaration = p.bufferStrides.find(binding);
        if (declaration == p.bufferStrides.end() || structured != bool(declaration->second))
            fail("DXBC memory instruction disagrees with its declaration");
        if (s.inputs.memory) {
            const auto view = s.inputs.memory->views_.find(binding);
            if (view != s.inputs.memory->views_.end() && view->second.resource &&
                view->second.stride != declaration->second)
                fail("DXBC buffer declaration disagrees with captured stride");
        }
        const auto indexValue = structured ? s.source(args[1], true) : Value::known({});
        const auto offsetValue = s.source(args[structured ? 2 : 1], true);
        if (!(indexValue.valid & offsetValue.valid & 1))
            fail("Unknown DXBC memory address");
        if (store) {
            if (resource.components != 2 || resource.mode != 0 || !s.inputs.memory)
                fail("Unavailable or invalid DXBC memory store");
            const auto value = s.source(args.back(), false);
            result.memoryChanges = s.inputs.memory->store(binding, structured, indexValue.bits[0],
                                                          offsetValue.bits[0], value, resource.mask);
        } else {
            const auto &destination = args[0];
            if (destination.modifier || destination.components != 2 || destination.mode != 0 ||
                (destination.kind != 0 && destination.kind != 2 && destination.kind != 3))
                fail("Invalid DXBC buffer load destination");
            const auto target = s.address(destination);
            const auto value =
                s.inputs.memory
                    ? s.inputs.memory->load(binding, structured, indexValue.bits[0], offsetValue.bits[0],
                                            resource.select, destination.mask)
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
        std::vector<Value> values;
        for (size_t i = destinations; i < args.size(); ++i)
            values.push_back(s.source(args[i], integerSources(op)));
        std::array<Value, 2> outputs{};
        FloatEnvironment environment;
        if (op >= 15 && op <= 17) {
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
        for (size_t i = 0; i < destinations; ++i) {
            const auto &target = targets[i];
            if (target.kind == 13 || !s.inBounds(target))
                continue;
            const auto previous = std::find_if(result.changes.begin(), result.changes.end(),
                                               [&](const Change &c) { return c.destination == target; });
            if (previous != result.changes.end()) {
                for (size_t j = 0; j < i; ++j)
                    if (targets[j] == target && (args[j].mask & args[i].mask))
                        fail("Overlapping DXBC multi-result destinations");
            }
            auto after = previous == result.changes.end() ? s.value(target) : previous->after;
            const auto before = after;
            for (unsigned c = 0; c < 4; ++c)
                if (args[i].mask & (1u << c)) {
                    if (target.kind == 3 && c >= p.arrays.at(target.indices[0]).second)
                        continue;
                    auto bits = outputs[i].bits[c];
                    if (instruction.words[0] & (1u << 13)) {
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
    for (const auto &change : result.changes)
        s.registers[change.destination] = change.after;
    if (!result.memoryChanges.empty())
        s.inputs.memory->commit(result.memoryChanges);
    s.calls = std::move(calls);
    s.pc = next;
    s.done = done;
    ++s.sequence;
    if (!done)
        result.nextInstruction = next;
    return result;
}
} // namespace flora::dxbc_exec
