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
    uint32_t stride = 0;             // Zero for raw views, structure size otherwise.
    std::optional<uint64_t> counter; // View identity, independent of backing resource identity.
    uint32_t format = 0;             // A native typed buffer format; zero for raw/structured views.
    auto operator<=>(const BufferView &) const = default;
};
enum class CounterKind { Ordered, Append };
struct Counter {
    CounterKind kind = CounterKind::Ordered;
    std::optional<uint32_t> value;
    auto operator<=>(const Counter &) const = default;
};
struct CounterChange {
    uint64_t view;
    Counter before, after;
};
struct MemoryChange {
    uint64_t resource, offset;
    std::vector<uint8_t> before, after, beforeValid, afterValid;
};
class GpuOperations;
struct TextureInputs;
class Memory {
    std::map<uint64_t, Buffer> buffers_;
    std::map<Register, BufferView> views_;
    std::map<uint64_t, Counter> counters_;
    uint64_t maxChangeBytes_;
    std::shared_ptr<GpuOperations> gpu_;
    bool assistAll_;
    friend class Lane;
    Value loadTyped(uint32_t stage, const Register &, uint32_t index, uint32_t returns,
                    const std::array<uint32_t, 4> &, uint32_t mask, bool literal, bool &assisted) const;
    std::vector<MemoryChange> storeTyped(const Register &, uint32_t index, uint32_t returns, const Value &,
                                         bool literal, bool &assisted) const;
    Value load(const Register &binding, bool structured, uint32_t index, uint32_t offset,
               const std::array<uint32_t, 4> &swizzle, uint32_t mask, bool literalIndex, bool literalOffset,
               bool &assisted) const;
    std::vector<MemoryChange> store(const Register &binding, bool structured, uint32_t index, uint32_t offset,
                                    const Value &value, uint32_t mask, bool literalIndex, bool literalOffset,
                                    bool &assisted) const;
    void commit(const std::vector<MemoryChange> &changes);
    void commitCounters(const std::vector<CounterChange> &changes);
    std::pair<Value, std::optional<CounterChange>> counter(const Register &, bool consume) const;
    std::vector<MemoryChange> invalidateAppend(const Register &, uint32_t index) const;
    std::vector<MemoryChange> invalidateShared() const;
    std::pair<Value, std::vector<MemoryChange>> atomic(const Register &, uint32_t opcode, uint32_t index,
                                                       uint32_t offset, const Value &value,
                                                       const Value &comparison) const;

  public:
    // This is private diagnostic storage, never a mapping of replay GPU memory.
    // Lanes may share it only under an external serial execution scheduler.
    Memory(std::map<uint64_t, Buffer> buffers, std::map<Register, BufferView> views,
           uint64_t maxBytes = 256 * 1024 * 1024, uint64_t maxChangeBytes = 16 * 1024 * 1024,
           std::shared_ptr<GpuOperations> gpu = {}, bool assistAll = false,
           std::map<uint64_t, Counter> counters = {});
    const std::map<uint64_t, Buffer> &buffers() const;
    const std::map<uint64_t, Counter> &counters() const;
};
struct Inputs {
    // Missing inputs and missing bindings remain unknown. Present constant
    // buffers have explicit lengths; reads beyond those lengths return zero.
    std::map<Register, Value> registers;
    std::map<uint32_t, std::vector<Value>> constantBuffers;
    std::shared_ptr<Memory> memory;
    std::shared_ptr<GpuOperations> gpu;
    std::shared_ptr<Memory> sharedMemory;
    std::shared_ptr<TextureInputs> textures;
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
    bool memoryGpuAssisted = false;
    bool instructionGpuAssisted = false;
    std::vector<MemoryChange> sharedMemoryChanges;
    uint32_t syncFlags = 0;
    std::vector<std::pair<uint32_t, uint64_t>> syncLoopIterations;
    bool atomic = false;
    std::vector<CounterChange> counterChanges;
    std::vector<MemoryChange> textureChanges;
};
struct SharedDeclaration {
    uint32_t bytes = 0, stride = 0;
};
struct ProgramData;
class Program {
    std::shared_ptr<const ProgramData> data_;
    friend class Lane;

  public:
    explicit Program(Bytes dxbc);
    const std::vector<Instruction> &instructions() const;
    uint32_t stage() const;
    std::array<uint32_t, 3> threadGroupSize() const;
    const std::map<uint32_t, SharedDeclaration> &sharedDeclarations() const;
};
class Lane {
    struct State;
    std::unique_ptr<State> state_;
    friend class Group;
    friend class PixelQuad;
    void releaseBarrier();
    bool samplingBoundary() const;
    bool hasPendingSampling() const;
    bool prepareSamplingBatch(const std::vector<Lane *> &);

  public:
    Lane(const Program &program, Inputs inputs, uint64_t maxSteps = 1000000);
    ~Lane();
    bool finished() const;
    bool blocked() const;
    Value value(const Register &reg) const;
    const std::map<Register, Value> &registers() const;
    // One instruction at a time: the caller owns trace storage and cancellation.
    // Unsupported semantics or unknown control/address inputs fail explicitly.
    Step step();
};
struct GroupStep {
    uint64_t sequence, barrierEpoch;
    uint32_t lane;
    Step step;
    bool releasedBarrier = false;
};
class Group {
    struct State;
    std::unique_ptr<State> state_;

  public:
    // One deterministic legal serialization, not the captured GPU schedule.
    // Shared storage starts unknown and is private to this group instance.
    Group(const Program &, Inputs, std::array<uint32_t, 3> groupId = {}, uint64_t maxSteps = 1000000,
          CancelCheck cancelled = {});
    ~Group();
    bool finished() const;
    GroupStep step();
    const Lane &lane(uint32_t index) const;
    const Memory &sharedMemory() const;
    uint64_t samplingBatches() const;
    static constexpr const char *executionModel = "bounded-sampling-rendezvous/3";
};
struct PixelQuadStep {
    uint64_t sequence;
    uint32_t lane;
    bool helper;
    uint8_t activeMask;
    Step step;
};
class PixelQuad {
    struct State;
    std::unique_ptr<State> state_;

  public:
    // The caller supplies proven row-major quad inputs. Coverage does not remove
    // helper lanes. Captured quad discovery and sample selection are separate.
    PixelQuad(const Program &, std::array<Inputs, 4>, uint8_t coverage = 15, uint64_t maxSteps = 1000000,
              CancelCheck cancelled = {});
    ~PixelQuad();
    bool finished() const;
    PixelQuadStep step();
    const Lane &lane(uint32_t index) const;
    uint8_t activeMask() const;
    uint64_t derivativeBatches() const;
    uint64_t samplingBatches() const;
    static constexpr const char *executionModel = "coherent-pixel-sampling/2";
};
} // namespace flora::dxbc_exec
