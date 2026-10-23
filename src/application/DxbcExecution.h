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
struct Buffer {
    std::vector<uint8_t> bytes, valid;
    static Buffer known(std::vector<uint8_t> bytes);
};
struct BufferView {
    // A present view with no resource is an explicitly unbound slot. Missing
    // views mean that binding information was not captured.
    std::optional<uint64_t> resource;
    uint64_t offset = 0, size = 0;
    uint32_t stride = 0; // Zero for raw views, structure size otherwise.
};
struct MemoryChange {
    uint64_t resource, offset;
    std::vector<uint8_t> before, after, beforeValid, afterValid;
};
class Memory {
    std::map<uint64_t, Buffer> buffers_;
    std::map<Register, BufferView> views_;
    uint64_t maxChangeBytes_;
    friend class Lane;
    Value load(const Register &binding, bool structured, uint32_t index, uint32_t offset,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask) const;
    std::vector<MemoryChange> store(const Register &binding, bool structured, uint32_t index, uint32_t offset,
                                    const Value &value, uint32_t mask) const;
    void commit(const std::vector<MemoryChange> &changes);

  public:
    // This is private diagnostic storage, never a mapping of replay GPU memory.
    // Lanes may share it only under an external serial execution scheduler.
    Memory(std::map<uint64_t, Buffer> buffers, std::map<Register, BufferView> views,
           uint64_t maxBytes = 256 * 1024 * 1024, uint64_t maxChangeBytes = 16 * 1024 * 1024);
    const std::map<uint64_t, Buffer> &buffers() const;
};
struct Inputs {
    // Missing inputs and missing bindings remain unknown. Present constant
    // buffers have explicit lengths; reads beyond those lengths return zero.
    std::map<Register, Value> registers;
    std::map<uint32_t, std::vector<Value>> constantBuffers;
    std::shared_ptr<Memory> memory;
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
    std::vector<MemoryChange> memoryChanges;
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
