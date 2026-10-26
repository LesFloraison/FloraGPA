#include "DxbcExecutionTraceReader.h"
#include <QCryptographicHash>
#include <QFile>
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#include <set>
#include <windows.h>

namespace flora::dxbc_exec {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(const char *message) { throw std::runtime_error(message); }
void require(bool condition, const char *message) {
    if (!condition)
        invalid(message);
}
uint64_t number(const Json &v, uint64_t maximum = UINT64_MAX) {
    require(v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0),
            "Trace requires an unsigned integer");
    const auto result = v.get<uint64_t>();
    require(result <= maximum, "Trace integer exceeds its range");
    return result;
}
bool boolean(const Json &v) {
    require(v.is_boolean(), "Trace requires a boolean");
    return v.get<bool>();
}
const Json &array(const Json &v) {
    require(v.is_array(), "Trace requires an array");
    return v;
}
Register reg(const Json &v) {
    Register r{uint32_t(number(v.at("dxbc_operand_kind"), UINT32_MAX)), {}};
    const auto &indices = array(v.at("indices"));
    require(indices.size() <= 3, "Too many trace register indices");
    for (const auto &index : indices)
        r.indices.push_back(uint32_t(number(index, UINT32_MAX)));
    return r;
}
Value value(const Json &v) {
    Value result;
    const auto &bits = array(v.at("bits"));
    require(bits.size() == 4, "Trace register must have four components");
    for (size_t i = 0; i < 4; ++i)
        result.bits[i] = uint32_t(number(bits[i], UINT32_MAX));
    result.valid = uint8_t(number(v.at("valid_components"), 15));
    return result;
}
Counter counter(const Json &v) {
    const auto &kind = v.at("kind");
    require(kind == "ordered" || kind == "append", "Unknown trace counter kind");
    Counter result{kind == "ordered" ? CounterKind::Ordered : CounterKind::Append, {}};
    if (!v.at("value").is_null())
        result.value = uint32_t(number(v.at("value"), UINT32_MAX));
    return result;
}
std::vector<uint8_t> hex(const Json &v, bool validity = false) {
    const auto &text = v.get_ref<const std::string &>();
    require(text.size() % 2 == 0, "Odd trace hex extent");
    std::vector<uint8_t> result(text.size() / 2);
    const auto digit = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9')
            return uint8_t(c - '0');
        if (c >= 'a' && c <= 'f')
            return uint8_t(c - 'a' + 10);
        invalid("Invalid trace hex digit");
    };
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = uint8_t(digit(text[2 * i]) * 16 + digit(text[2 * i + 1]));
        require(!validity || result[i] <= 1, "Invalid trace storage validity");
    }
    return result;
}
TraceSpace space(const Json &v) {
    if (v == "buffer")
        return TraceSpace::Buffer;
    if (v == "shared")
        return TraceSpace::Shared;
    if (v == "texture")
        return TraceSpace::Texture;
    invalid("Unknown trace storage space");
}
struct ReadLock {
    int descriptor = -1;
    explicit ReadLock(const QString &path) {
        const auto handle =
            CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Cannot lock execution trace for reading");
        descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
        if (descriptor == -1) {
            CloseHandle(handle);
            invalid("Cannot attach the execution trace handle");
        }
    }
    ~ReadLock() { _close(descriptor); }
};
} // namespace

struct ExecutionTraceReader::State {
    ReadLock lock;
    mutable QFile file;
    TraceReadLimits limits;
    Json header, completion;
    qint64 sourceOffset = -1;
    struct InstructionIndex {
        qint64 offset;
        uint32_t wordOffset, opcode;
        bool declaration;
    };
    std::vector<InstructionIndex> instructions;
    std::vector<qint64> steps, observations;
    std::vector<uint64_t> selected;
    std::map<TraceRegisterKey, Value> registers;
    std::map<TraceRegisterKey, uint64_t> introduced;
    std::map<std::pair<uint32_t, uint32_t>, Value> constants;
    std::map<uint32_t, uint64_t> constantLengths;
    std::map<TraceStorageKey, Buffer> storage;
    std::map<uint64_t, Counter> counters;
    uint64_t position = 0;
    uint32_t lanes = 0, selectedLane = 0;
    bool failed = false;

    State(const QString &path, const TraceReadLimits &l) : lock(path), file(path), limits(l) {
        require(l.maxBytes && l.maxRecordBytes && l.maxRecords && l.maxSteps && l.maxRegisters,
                "Trace reader limits must be nonzero");
        require(l.maxBytes < uint64_t(INT64_MAX) && l.maxRecordBytes < uint64_t(INT64_MAX) - 1,
                "Trace reader byte limits exceed file offsets");
        require(file.open(lock.descriptor, QIODevice::ReadOnly, QFileDevice::DontCloseHandle),
                "Cannot open the locked execution trace handle");
        require(file.size() > 0 && uint64_t(file.size()) <= l.maxBytes, "Trace file byte budget exceeded");
    }
    void healthy() const { require(!failed, "Execution trace reader requires reloading after failure"); }
    QByteArray line() const {
        const auto bytes = file.readLine(qint64(limits.maxRecordBytes) + 1);
        require(!bytes.isEmpty() && bytes.endsWith('\n') && uint64_t(bytes.size()) <= limits.maxRecordBytes,
                "Truncated trace record or record byte budget exceeded");
        return bytes;
    }
    Json parse(const QByteArray &bytes) const {
        std::vector<std::set<std::string>> keys;
        auto result = Json::parse(bytes.constData(), bytes.constData() + bytes.size(),
                                  [&](int depth, Json::parse_event_t event, Json &v) {
                                      require(depth <= 64, "Trace JSON nesting exceeds its budget");
                                      if (event == Json::parse_event_t::object_start)
                                          keys.emplace_back();
                                      else if (event == Json::parse_event_t::key)
                                          require(keys.back().insert(v.get<std::string>()).second,
                                                  "Duplicate trace JSON key");
                                      else if (event == Json::parse_event_t::object_end)
                                          keys.pop_back();
                                      return true;
                                  });
        require(result.is_object(), "Trace record must be an object");
        return result;
    }
    Json read(qint64 offset) const {
        healthy();
        require(file.seek(offset), "Cannot seek execution trace");
        return parse(line());
    }
    uint32_t lane(const Json &v) const { return uint32_t(number(v, lanes - 1)); }
    void apply(const Json &row, bool forward, uint64_t sequence) {
        const auto laneId = lane(row.at("lane"));
        const char *prior = forward ? "before" : "after", *next = forward ? "after" : "before";
        const auto visit = [&](const Json &items, const auto &operation) {
            array(items);
            if (forward)
                for (const auto &item : items)
                    operation(item);
            else
                for (auto it = items.rbegin(); it != items.rend(); ++it)
                    operation(*it);
        };
        visit(row.at("register_changes"), [&](const Json &c) {
            const TraceRegisterKey key{laneId, reg(c.at("register"))};
            auto it = registers.find(key);
            const auto before = value(c.at(prior)), after = value(c.at(next));
            require((it == registers.end() ? Value{} : it->second) == before,
                    "Trace register chain mismatch");
            if (it == registers.end()) {
                require(forward && registers.size() < limits.maxRegisters, "Trace register budget exceeded");
                introduced.try_emplace(key, sequence);
                it = registers.emplace(key, before).first;
            }
            it->second = after;
        });
        // Remove registers introduced by this instruction only after all of its
        // reverse deltas, including repeated writes to the same destination.
        if (!forward)
            for (const auto &c : row.at("register_changes")) {
                const TraceRegisterKey key{laneId, reg(c.at("register"))};
                const auto it = introduced.find(key);
                if (it != introduced.end() && it->second == sequence)
                    registers.erase(key);
            }
        for (const char *name : {"buffer", "shared", "texture"})
            visit(row.at(std::string(name) + "_changes"), [&](const Json &c) {
                auto &b = storage.at({space(name), number(c.at("resource"))});
                const auto offset = number(c.at("offset"), b.bytes.size());
                const auto before = hex(c.at(std::string(prior) + "_hex"));
                const auto after = hex(c.at(std::string(next) + "_hex"));
                const auto beforeValid = hex(c.at(std::string(prior) + "_valid_hex"), true);
                const auto afterValid = hex(c.at(std::string(next) + "_valid_hex"), true);
                require(before.size() == after.size() && before.size() == beforeValid.size() &&
                            before.size() == afterValid.size() && before.size() <= b.bytes.size() - offset,
                        "Trace storage delta extent mismatch");
                require(std::equal(before.begin(), before.end(), b.bytes.begin() + offset) &&
                            std::equal(beforeValid.begin(), beforeValid.end(), b.valid.begin() + offset),
                        "Trace storage chain mismatch");
                std::copy(after.begin(), after.end(), b.bytes.begin() + offset);
                std::copy(afterValid.begin(), afterValid.end(), b.valid.begin() + offset);
            });
        visit(row.at("counter_changes"), [&](const Json &c) {
            auto &current = counters.at(number(c.at("view")));
            require(current == counter(c.at(prior)), "Trace counter chain mismatch");
            current = counter(c.at(next));
        });
    }
    void move(uint64_t target, const CancelCheck &cancelled) {
        healthy();
        require(target <= steps.size(), "Trace position is out of range");
        while (position != target) {
            checkCancellation(cancelled);
            const bool forward = position < target;
            const auto index = forward ? position : position - 1;
            try {
                apply(read(steps.at(index)), forward, index);
                position = forward ? position + 1 : position - 1;
            } catch (...) {
                failed = true;
                throw;
            }
        }
    }
};

ExecutionTraceReader::ExecutionTraceReader(const QString &path, const std::string &expectedSha256,
                                           const TraceReadLimits &limits, const CancelCheck &cancelled)
    : state_(std::make_unique<State>(path, limits)) {
    auto &s = *state_;
    require(expectedSha256.size() == 64 &&
                std::all_of(expectedSha256.begin(), expectedSha256.end(),
                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
            "Trace requires a lowercase SHA-256 artifact digest");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    uint64_t records = 0, storageBytes = 0, wordOffset = 2, epoch = 0;
    bool begun = false, completed = false;
    uint8_t pixelActive = 0;
    std::map<TraceStorageKey, uint64_t> filled;
    std::vector<uint64_t> laneSteps;
    std::vector<std::optional<uint32_t>> nextInstructions;
    const auto finishInitial = [&] {
        require(s.sourceOffset >= 0 && !s.instructions.empty(), "Trace source or instructions are missing");
        for (const auto &[key, b] : s.storage)
            require(filled.at(key) == b.bytes.size(), "Incomplete trace initial storage");
        if (s.header.contains("constant_buffer_lengths")) {
            std::map<uint32_t, uint64_t> counts;
            for (const auto &[key, v] : s.constants) {
                require(s.constantLengths.contains(key.first) && key.second < s.constantLengths.at(key.first),
                        "Trace constant lies outside its declared buffer");
                ++counts[key.first];
            }
            for (const auto &[slot, length] : s.constantLengths)
                require(counts[slot] == length, "Incomplete trace constant buffer");
        }
    };
    while (!s.file.atEnd()) {
        checkCancellation(cancelled);
        require(++records <= limits.maxRecords && !completed,
                "Trace record budget or completion boundary exceeded");
        const auto offset = s.file.pos();
        const auto bytes = s.line();
        hash.addData(bytes);
        const auto row = s.parse(bytes);
        const auto kind = row.at("record").get<std::string>();
        if (records == 1) {
            require(kind == "header" && row.at("format") == "flora.dxbc-execution-trace" &&
                        number(row.at("version")) == 1 &&
                        ((row.at("stage") == "cs" && row.at("scope") == "one_cooperative_group") ||
                         (row.at("stage") == "vs" && row.at("scope") == "one_vertex") ||
                         (row.at("stage") == "ps" && row.at("scope") == "one_pixel_quad")) &&
                        !boolean(row.at("captured_gpu_schedule")),
                    "Unsupported execution trace header");
            require(number(row.at("max_steps")) > 0 && number(row.at("max_bytes")) >= uint64_t(s.file.size()),
                    "Trace exceeds its declared limits");
            if (row.at("stage") == "vs") {
                require(row.at("execution_model") == "independent-vertex-invocation/1",
                        "Unsupported vertex trace execution model");
                require(!row.contains("group_size") && !row.contains("group") && !row.contains("thread") &&
                            number(row.at("selected_lane")) == 0,
                        "Invalid vertex trace invocation");
                number(row.at("input_reference"), UINT32_MAX);
                number(row.at("instance"), UINT32_MAX);
                if (row.contains("index_override") && !row.at("index_override").is_null())
                    number(row.at("index_override"), UINT32_MAX);
                s.lanes = 1;
                s.selectedLane = 0;
            } else if (row.at("stage") == "ps") {
                require(row.at("execution_model") == PixelQuad::executionModel &&
                            !row.contains("group_size") && !row.contains("group") &&
                            !row.contains("thread") && row.at("quad_input_provenance").is_object(),
                        "Invalid pixel trace identity");
                s.lanes = 4;
                s.selectedLane = uint32_t(number(row.at("selected_lane"), 3));
                pixelActive = uint8_t(number(row.at("initial_active_mask"), 15));
            } else {
                const auto &size = array(row.at("group_size")), &thread = array(row.at("thread"));
                require(size.size() == 3 && thread.size() == 3 && array(row.at("group")).size() == 3,
                        "Invalid trace group dimensions");
                uint64_t lanes = 1, selected = 0;
                for (size_t i = 0; i < 3; ++i) {
                    const auto dimension = number(size[i], 1024);
                    require(dimension > 0, "Empty trace group dimension");
                    selected += lanes * number(thread[i], dimension - 1);
                    lanes *= dimension;
                    require(lanes <= 1024, "Trace exceeds the DX11 group lane limit");
                    number(row.at("group")[i], UINT32_MAX);
                }
                require(number(row.at("selected_lane")) == selected,
                        "Trace selected lane disagrees with thread");
                s.lanes = uint32_t(lanes);
                s.selectedLane = uint32_t(selected);
            }
            laneSteps.resize(s.lanes);
            nextInstructions.resize(s.lanes);
            s.header = row;
            if (row.contains("constant_buffer_lengths"))
                for (const auto &c : array(row.at("constant_buffer_lengths"))) {
                    const auto slot = uint32_t(number(c.at("slot"), UINT32_MAX));
                    require(s.constantLengths.size() < limits.maxRegisters &&
                                s.constantLengths.emplace(slot, number(c.at("elements"), limits.maxRegisters))
                                    .second,
                            "Duplicate or excessive trace constant buffers");
                }
            continue;
        }
        if (kind == "step" || kind == "complete") {
            if (!begun) {
                finishInitial();
                begun = true;
            }
            if (kind == "complete") {
                require(boolean(row.at("completed")) && number(row.at("steps")) == s.steps.size() &&
                            number(row.at("selected_steps")) == s.selected.size(),
                        "Trace completion counts disagree");
                for (size_t i = 0; i < laneSteps.size(); ++i)
                    require(laneSteps[i] && !nextInstructions[i], "Trace completion has an unfinished lane");
                if (s.header.at("stage") == "ps") {
                    require(number(row.at("final_active_mask"), 15) == pixelActive,
                            "Pixel trace terminal active mask differs");
                    number(row.at("derivative_batches"), s.steps.size());
                    number(row.at("sampling_batches"), s.steps.size());
                }
                s.completion = row;
                completed = true;
                continue;
            }
            const auto sequence = s.steps.size();
            require(sequence < limits.maxSteps && sequence < number(s.header.at("max_steps")) &&
                        number(row.at("sequence")) == sequence,
                    "Trace global sequence or step budget mismatch");
            const auto lane = s.lane(row.at("lane"));
            require(number(row.at("lane_sequence")) == laneSteps[lane], "Trace lane sequence mismatch");
            const auto instruction = uint32_t(number(row.at("instruction"), s.instructions.size() - 1));
            require(!s.instructions[instruction].declaration &&
                        number(row.at("word_offset")) == s.instructions[instruction].wordOffset,
                    "Trace step instruction identity mismatch");
            if (laneSteps[lane])
                require(nextInstructions[lane] && *nextInstructions[lane] == instruction,
                        "Trace lane instruction chain mismatch");
            else {
                const auto first = std::find_if(s.instructions.begin(), s.instructions.end(),
                                                [](const auto &i) { return !i.declaration; });
                require(instruction == size_t(first - s.instructions.begin()),
                        "Trace lane starts at the wrong instruction");
            }
            const auto target = [&](const Json &v) {
                const auto index = uint32_t(number(v, s.instructions.size() - 1));
                require(!s.instructions[index].declaration, "Trace instruction target is a declaration");
                return index;
            };
            nextInstructions[lane] = row.at("next_instruction").is_null()
                                         ? std::nullopt
                                         : std::optional<uint32_t>(target(row.at("next_instruction")));
            for (const auto &index : array(row.at("return_instruction_stack")))
                target(index);
            for (const auto &loop : array(row.at("sync_loop_iterations"))) {
                require(array(loop).size() == 2, "Invalid trace loop epoch");
                target(loop[0]);
                number(loop[1]);
            }
            require(number(row.at("barrier_epoch")) == epoch, "Trace barrier epoch mismatch");
            epoch += boolean(row.at("released_barrier"));
            for (const char *flag :
                 {"discarded", "atomic", "memory_gpu_assisted", "instruction_gpu_assisted"})
                boolean(row.at(flag));
            number(row.at("sync_flags"), UINT32_MAX);
            if (s.header.at("stage") == "vs")
                require(!boolean(row.at("discarded")) && !boolean(row.at("released_barrier")) &&
                            number(row.at("barrier_epoch")) == 0 && number(row.at("sync_flags")) == 0 &&
                            array(row.at("shared_changes")).empty(),
                        "Vertex trace contains pixel or group execution state");
            if (s.header.at("stage") == "ps") {
                const bool helper = !(pixelActive & (1u << lane));
                require(boolean(row.at("helper_before")) == helper && !boolean(row.at("released_barrier")) &&
                            number(row.at("barrier_epoch")) == 0 && number(row.at("sync_flags")) == 0 &&
                            array(row.at("shared_changes")).empty(),
                        "Pixel trace helper or group state differs");
                if (boolean(row.at("discarded"))) {
                    require(s.instructions[instruction].opcode == 13,
                            "Pixel trace discard is not a discard instruction");
                    pixelActive &= uint8_t(~(1u << lane));
                }
                require(number(row.at("active_mask_after"), 15) == pixelActive,
                        "Pixel trace active mask transition differs");
                if (helper)
                    require(array(row.at("buffer_changes")).empty() &&
                                array(row.at("texture_changes")).empty() &&
                                array(row.at("counter_changes")).empty(),
                            "Pixel helper trace contains side effects");
            }
            s.apply(row, true, sequence);
            s.steps.push_back(offset);
            ++s.position;
            ++laneSteps[lane];
            if (lane == s.selectedLane)
                s.selected.push_back(sequence);
            continue;
        }
        require(!begun, "Initial trace record follows execution");
        if (kind == "source") {
            require(s.sourceOffset < 0 && row.at("data").is_object(), "Duplicate or invalid trace source");
            s.sourceOffset = offset;
        } else if (kind == "instruction") {
            require(number(row.at("index")) == s.instructions.size() &&
                        number(row.at("word_offset"), UINT32_MAX) == wordOffset,
                    "Trace instruction sequence mismatch");
            const auto &words = array(row.at("words"));
            require(!words.empty() && words.size() <= UINT32_MAX - wordOffset,
                    "Invalid trace instruction size");
            for (const auto &word : words)
                number(word, UINT32_MAX);
            require((number(words[0]) & 0x7ff) == number(row.at("dxbc_opcode"), 0x7ff),
                    "Trace opcode disagrees with instruction word");
            s.instructions.push_back({offset, uint32_t(wordOffset), uint32_t(number(row.at("dxbc_opcode"))),
                                      boolean(row.at("declaration"))});
            wordOffset += words.size();
            row.at("source");
        } else if (kind == "register") {
            require(s.registers.size() < limits.maxRegisters &&
                        s.registers
                            .emplace(TraceRegisterKey{s.lane(row.at("lane")), reg(row.at("register"))},
                                     value(row.at("value")))
                            .second,
                    "Duplicate or excessive trace registers");
        } else if (kind == "constant") {
            require(s.constants.size() < limits.maxRegisters &&
                        s.constants
                            .emplace(std::make_pair(uint32_t(number(row.at("slot"), UINT32_MAX)),
                                                    uint32_t(number(row.at("element"), UINT32_MAX))),
                                     value(row.at("value")))
                            .second,
                    "Duplicate or excessive trace constants");
        } else if (kind == "counter") {
            require(s.counters.size() < limits.maxRegisters &&
                        s.counters.emplace(number(row.at("view")), counter(row.at("value"))).second,
                    "Duplicate or excessive trace counters");
        } else if (kind == "storage") {
            const auto size = number(row.at("bytes"), limits.maxStorageBytes - storageBytes);
            const TraceStorageKey key{space(row.at("space")), number(row.at("resource"))};
            require(s.header.at("stage") == "cs" || key.first != TraceSpace::Shared,
                    "Vertex trace contains group shared storage");
            require(s.storage.size() < limits.maxRegisters && !s.storage.contains(key),
                    "Duplicate or excessive trace storage");
            s.storage.emplace(key, Buffer{std::vector<uint8_t>(size), std::vector<uint8_t>(size)});
            filled.emplace(key, 0);
            storageBytes += size;
        } else if (kind == "storage_bytes") {
            const TraceStorageKey key{space(row.at("space")), number(row.at("resource"))};
            auto &b = s.storage.at(key);
            auto &next = filled.at(key);
            require(number(row.at("offset")) == next, "Trace initial storage gap or overlap");
            const auto bits = hex(row.at("bits_hex")), valid = hex(row.at("valid_hex"), true);
            require(!bits.empty() && bits.size() == valid.size() && bits.size() <= b.bytes.size() - next,
                    "Trace initial storage extent mismatch");
            std::copy(bits.begin(), bits.end(), b.bytes.begin() + next);
            std::copy(valid.begin(), valid.end(), b.valid.begin() + next);
            next += bits.size();
        } else {
            static const std::set<std::string> observations{"binding",
                                                            "unavailable_binding",
                                                            "unavailable_counter",
                                                            "texture_metadata",
                                                            "sampler",
                                                            "sample_queries",
                                                            "texture_view",
                                                            "multisample_observation",
                                                            "multisample_values",
                                                            "texture_subresource"};
            require(observations.contains(kind), "Unknown trace record kind");
            s.observations.push_back(offset);
        }
    }
    require(completed, "Trace completion record is missing");
    require(hash.result().toHex().toStdString() == expectedSha256, "Trace artifact SHA-256 mismatch");
    s.move(0, cancelled);
}
ExecutionTraceReader::~ExecutionTraceReader() = default;
nlohmann::json ExecutionTraceReader::header() const {
    state_->healthy();
    return state_->header;
}
nlohmann::json ExecutionTraceReader::source() const { return state_->read(state_->sourceOffset).at("data"); }
nlohmann::json ExecutionTraceReader::completion() const {
    state_->healthy();
    return state_->completion;
}
nlohmann::json ExecutionTraceReader::instruction(uint64_t i) const {
    return state_->read(state_->instructions.at(i).offset);
}
nlohmann::json ExecutionTraceReader::step(uint64_t i) const { return state_->read(state_->steps.at(i)); }
size_t ExecutionTraceReader::observationCount() const {
    state_->healthy();
    return state_->observations.size();
}
nlohmann::json ExecutionTraceReader::observation(size_t i) const {
    return state_->read(state_->observations.at(i));
}
uint64_t ExecutionTraceReader::instructionCount() const {
    state_->healthy();
    return state_->instructions.size();
}
uint64_t ExecutionTraceReader::size() const {
    state_->healthy();
    return state_->steps.size();
}
uint64_t ExecutionTraceReader::position() const {
    state_->healthy();
    return state_->position;
}
uint64_t ExecutionTraceReader::selectedSize() const {
    state_->healthy();
    return state_->selected.size();
}
uint64_t ExecutionTraceReader::selectedBoundary(uint64_t i) const {
    state_->healthy();
    return i == state_->selected.size() ? state_->steps.size() : state_->selected.at(i);
}
void ExecutionTraceReader::moveTo(uint64_t target, const CancelCheck &cancelled) {
    state_->move(target, cancelled);
}
Value ExecutionTraceReader::registerValue(uint32_t lane, const Register &reg) const {
    state_->healthy();
    require(lane < state_->lanes, "Trace register lane is out of range");
    const auto it = state_->registers.find({lane, reg});
    return it == state_->registers.end() ? Value{} : it->second;
}
Value ExecutionTraceReader::constant(uint32_t slot, uint32_t element) const {
    state_->healthy();
    const auto it = state_->constants.find({slot, element});
    if (it != state_->constants.end())
        return it->second;
    const auto length = state_->constantLengths.find(slot);
    return length != state_->constantLengths.end() && element >= length->second ? Value::known({}) : Value{};
}
const std::map<TraceRegisterKey, Value> &ExecutionTraceReader::registers() const {
    state_->healthy();
    return state_->registers;
}
const std::map<std::pair<uint32_t, uint32_t>, Value> &ExecutionTraceReader::constants() const {
    state_->healthy();
    return state_->constants;
}
const std::map<TraceStorageKey, Buffer> &ExecutionTraceReader::storage() const {
    state_->healthy();
    return state_->storage;
}
const std::map<uint64_t, Counter> &ExecutionTraceReader::counters() const {
    state_->healthy();
    return state_->counters;
}
} // namespace flora::dxbc_exec
