#pragma once
#include "core/Frame.h"
#include <compare>
#include <memory>
#include <optional>

namespace flora::dxbc_exec {
struct Value {
    std::array<uint32_t, 4> bits{};
    uint8_t valid = 0;
    auto operator<=>(const Value &) const = default;
    static Value known(std::array<uint32_t, 4> bits) { return {bits, 15}; }
};
struct Register {
    uint32_t kind = 0;
    std::vector<uint32_t> indices;
    auto operator<=>(const Register &) const = default;
};
struct Instruction {
    uint32_t opcode, wordOffset;
    bool declaration;
    std::vector<uint32_t> words;
};
struct Inputs {
    // Missing inputs and missing bindings remain unknown. Present constant
    // buffers have explicit lengths; reads beyond those lengths return zero.
    std::map<Register, Value> registers;
    std::map<uint32_t, std::vector<Value>> constantBuffers;
};
struct Change {
    Register destination;
    Value before, after;
};
struct Step {
    uint64_t sequence;
    uint32_t instruction, wordOffset;
    std::optional<uint32_t> nextInstruction;
    std::vector<uint32_t> callstack;
    std::vector<Change> changes;
    bool discarded = false;
};
struct ProgramData;
class Program {
    std::shared_ptr<const ProgramData> data_;
    friend class Lane;

  public:
    explicit Program(Bytes dxbc);
    const std::vector<Instruction> &instructions() const;
    uint32_t stage() const;
};
class Lane {
    struct State;
    std::unique_ptr<State> state_;

  public:
    Lane(const Program &program, Inputs inputs, uint64_t maxSteps = 1000000);
    ~Lane();
    bool finished() const;
    Value value(const Register &reg) const;
    const std::map<Register, Value> &registers() const;
    // One instruction at a time: the caller owns trace storage and cancellation.
    // Unsupported semantics or unknown control/address inputs fail explicitly.
    Step step();
};
} // namespace flora::dxbc_exec
