#include "DxbcCheckpoint.h"
#include "DxbcCheckpointModel.h"
#include "InvocationSelector.h"
#include <algorithm>
#include <bit>

namespace flora {
namespace {
using namespace checkpoint;
constexpr uint32_t SHEX = 0x58454853, SFI0 = 0x30494653;
constexpr uint32_t ifNonzero = 31 | (1 << 18);
const std::set<uint32_t> noDestination{2,   3,   4,   5,   6,   7,   8,   9,   10,  13,  18,  19,  20,  21,
                                       22,  23,  31,  44,  48,  58,  62,  63,  76,  117, 118, 119, 120, 164,
                                       166, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 190, 207, 208};
const std::set<uint32_t> twoDestinations{38, 77, 78, 81, 132, 133, 142};
const std::map<uint32_t, uint32_t> emissions{{19, 1}, {9, 2}, {20, 3}, {117, 1}, {118, 2}, {119, 3}};
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
Words op(uint32_t code, std::initializer_list<Words> args = {}) {
    Words result{code};
    for (const auto &arg : args)
        result.insert(result.end(), arg.begin(), arg.end());
    if (result.size() > 127)
        fail("Checkpoint instruction length exceeds DXBC limit");
    result[0] |= uint32_t(result.size()) << 24;
    return result;
}
Words dst(uint32_t reg, uint32_t mask = 15, uint32_t kind = 0) {
    return {0x100002 | (kind << 12) | (mask << 4), reg};
}
Words src(uint32_t reg, std::optional<uint32_t> component = {}) {
    return {component ? 0x10000a | (*component << 4) : 0x100e46, reg};
}
Words imm(uint32_t value) { return {0x4001, value}; }
uint32_t swizzle(uint32_t mask) {
    if (!mask || mask > 15)
        fail("Invalid checkpoint component mask");
    const auto first = std::countr_zero(mask);
    uint32_t value = 0;
    for (unsigned c = 0; c < 4; ++c)
        value |= ((mask & (1u << c)) ? c : first) << (c * 2);
    return value;
}
Words slice(const Words &words, size_t a, size_t b) { return {words.begin() + a, words.begin() + b}; }
Rows sliceRows(const Rows &rows, size_t a, size_t b) { return {rows.begin() + a, rows.begin() + b}; }
void append(Rows &to, const Rows &from) { to.insert(to.end(), from.begin(), from.end()); }

class CheckpointProgram {
    const CheckpointOptions &options;
    Program parsed;
    dxbc_detail::Parts parts;
    Rows original, ops, result;
    Json catalog, phases, phase = nullptr, inputSlots = Json::array(), registerSlots = Json::array(),
                          known = Json::object(), selector = nullptr, runtimeTerms = Json::array();
    std::vector<std::pair<uint32_t, Words>> identities;
    std::string domain;
    Arrays arrays;
    std::map<uint32_t, std::array<uint32_t, 2>> privateArrays;
    std::map<uint32_t, uint32_t> arraySizes;
    CallGraph calls;
    std::set<size_t> selected, staged;
    size_t split = 0;
    std::optional<uint32_t> token;
    bool trace, shared = false, cacheInputs = false, filtering = false, hasCalls = false;
    uint32_t nt = 0, no = 0, shadow = 0, added = 0, arrayElements = 0, helperLabel = 0, stride = 0,
             headerBytes = 0, inputCount = 0, values = 0, flags = 0, control = 0, ticket = 0, address = 0,
             inputCache = 0, callDepth = 0, indexCache = 0, safeIndices = 0, pairResults = 0, selection = 0,
             outputIndex = 0, outputValue = 0;
    uint64_t total = 0;
    void emit(uint32_t code, std::initializer_list<Words> args = {}) { result.push_back(op(code, args)); }
    Rows wrap(uint32_t reg, uint32_t component, uint32_t offset) const {
        return {op(80, {dst(ticket, 4), src(reg, component), imm(UINT32_MAX)}),
                op(ifNonzero, {src(ticket, 2)}), op(166, {dst(options.slot, 1, 30), imm(offset), imm(1)}),
                op(21)};
    }
    void store(Rows &body, uint32_t offset, const Words &operand, bool vector = false) const {
        body.push_back(op(30, {dst(address, 2), src(address, 0), imm(offset)}));
        body.push_back(op(166, {dst(options.slot, vector ? 15 : 1, 30), src(address, 1), operand}));
    }
    void declaredInputs() {
        auto decl = sliceRows(ops, 0, split);
        DeclaredInputs inputs;
        if (options.stage == "ds")
            inputs = domainInputs(decl);
        else if (!phase.is_null())
            inputs = hullInputs(sliceRows(original, 0, phases.at(0).at("start")), decl, phase);
        else {
            std::map<std::pair<uint32_t, uint32_t>, uint32_t> masks;
            for (const auto &row : decl) {
                const auto opcode = row.at(0) & 2047;
                if (opcode == 95 && row.size() == 2) {
                    if (row[1] == 0xb000) {
                        inputs.known["primitive"] = {{"operand", "vPrim"}, {"offset", 8}};
                        inputs.identity.push_back(Json::array({8, Json::array({0xb001})}));
                    } else if (row[1] == 0x25000) {
                        inputs.known["gs_instance"] = {{"operand", "vGSInstanceID"}, {"offset", 12}};
                        inputs.identity.push_back(Json::array({12, Json::array({0x2500a})}));
                    }
                }
                if ((opcode == 95 || opcode == 96 || opcode == 97) && ((row.at(1) >> 12) & 255) == 1) {
                    const auto t = row[1];
                    if ((t >> 31) || ((t >> 20) & 3) != 2 || ((t >> 22) & 63) || (t & 3) != 2 ||
                        ((t >> 2) & 3) || !row.at(2) || row[2] > 6)
                        fail("Unsupported checkpoint GS input declaration");
                    for (uint32_t vertex = 0; vertex < row[2]; ++vertex)
                        masks[{vertex, row.at(3)}] |= (t >> 4) & 15;
                }
            }
            for (const auto &[key, mask] : masks) {
                const auto [v, r] = key;
                inputs.inputSlots.push_back(
                    {{"name", "v[" + std::to_string(v) + "][" + std::to_string(r) + "]"},
                     {"kind", "input"},
                     {"vertex", v},
                     {"register", r},
                     {"mask", mask}});
            }
        }
        inputSlots = std::move(inputs.inputSlots);
        known = std::move(inputs.known);
        domain = std::move(inputs.domain);
        for (const auto &identity : inputs.identity)
            identities.emplace_back(identity.at(0).get<uint32_t>(), identity.at(1).get<Words>());
        inputCount = uint32_t(inputSlots.size());
    }
    void initialize(Bytes raw) {
        token = options.token;
        auto phaseId = options.hullPhase;
        if (options.stage == "hs") {
            phases = hullPhases(ops);
            if (!trace) {
                if (*token >= catalog.size() || !catalog.at(*token).at("checkpoint_allowed").get<bool>())
                    fail("HS checkpoint must identify an original executable instruction");
                const auto inferred = catalog.at(*token).at("hs_phase").get<uint32_t>();
                if (phaseId && *phaseId != inferred)
                    fail("HS checkpoint does not belong to the selected phase");
                phaseId = inferred;
            }
            if (!phaseId || *phaseId >= phases.size())
                fail("HS trace requires an explicit original phase ID");
            phase = phases.at(*phaseId);
            const auto start = phase.at("start").get<size_t>() + 1, end = phase.at("end").get<size_t>();
            ops = sliceRows(original, start, end);
            catalog = Json(catalog.begin() + start, catalog.begin() + end);
            split = phase.at("split").get<size_t>() - start;
            for (const auto &row : ops)
                if (const auto opcode = row[0] & 2047; opcode == 4 || opcode == 5 || opcode == 44)
                    fail("Original HS subroutine tracing is not supported");
            if (token)
                *token -= uint32_t(start);
        } else if (phaseId)
            fail("Phase selection only applies to HS");
        calls = subroutines(ops, split);
        hasCalls = !calls.targets.empty();
        shared = trace && calls.depth < 32 && phase.is_null();
        cacheInputs = trace || hasCalls || options.stage == "ds" || options.stage == "hs";
        if (!options.inputSelector.is_null() && !trace)
            fail("Input selection requires a complete invocation trace");
        while (calls.labels.contains(helperLabel))
            ++helperLabel;
        for (auto &entry : catalog) {
            // Preserve original token IDs (including the reference's HS catalog convention).
            const auto i = entry.at("token").get<size_t>();
            if (auto found = calls.owners.find(i); found != calls.owners.end())
                entry["function_label"] = found->second ? Json(*found->second) : Json(nullptr);
            if (auto found = calls.targets.find(i); found != calls.targets.end())
                entry["call_target"] = found->second;
        }
        if (!trace && (*token >= ops.size() || !catalog.at(*token).at("checkpoint_allowed").get<bool>()))
            fail("Checkpoint must identify an executable non-structural token");
        for (size_t i = 0; i < catalog.size(); ++i)
            if ((trace || !phase.is_null()) ? catalog[i].at("checkpoint_allowed").get<bool>() : i == *token)
                selected.insert(i);
        if ((trace || hasCalls) && (ops.empty() || (ops.back()[0] & 2047) != 62))
            fail("Shader trace requires a complete program ending in RET");
        if (options.slot >= 64 || !options.capacity)
            fail("Checkpoint UAV slot or capacity out of range");
        if ((parsed.code.header[0] & 255) == 0x50 &&
            dxbc_detail::uavSlots(parsed.code, parts).contains(options.slot))
            fail("Checkpoint UAV collides with an original declaration");
        for (const auto &row : ops)
            if ((row[0] & 2047) == 120 || (row[0] & 2047) >= 218)
                fail("Checkpoint does not support interface calls or feedback instructions");
        result = sliceRows(ops, 0, split);
        arrays = indexableDeclarations(result);
        uint64_t elements = 0;
        std::set<uint32_t> used;
        for (const auto &array : arrays) {
            elements += array.elements;
            arraySizes[array.array] = array.elements;
            used.insert(array.array);
        }
        if (elements > 4096 / 3)
            fail("Checkpoint exceeds temporary register limits");
        arrayElements = uint32_t(elements);
        size_t indexSlots = 0;
        for (size_t i = split; i < ops.size(); ++i) {
            if (!arrays.empty()) {
                size_t count = 0;
                for (const auto &[a, b] : instructionOperands(ops[i]).ranges)
                    count += indexableAccesses(slice(ops[i], a, b), arrays).size();
                indexSlots = std::max(indexSlots, count);
            }
            if (dependentResult(ops[i], arrays)) {
                staged.insert(i);
                const auto args = instructionOperands(ops[i]);
                const auto opcode = ops[i][0] & 2047;
                if ((opcode == 132 || opcode == 133) &&
                    ((ops[i][args.ranges[0].first] >> 4) & 15) != ((ops[i][args.ranges[1].first] >> 4) & 15))
                    fail("Dependent carry/borrow results with different masks have device-dependent writes");
            }
        }
        for (const auto &array : arrays) {
            std::array<uint32_t, 2> ids{};
            for (auto &id : ids) {
                while (used.contains(id))
                    ++id;
                used.insert(id);
            }
            privateArrays[array.array] = ids;
        }
        std::optional<size_t> temp;
        for (size_t i = 0; i < result.size(); ++i)
            if ((result[i][0] & 2047) == 104) {
                if (temp || result[i].size() != 2)
                    fail("Multiple or malformed shader temporary declarations");
                temp = i;
                nt = result[i][1];
            }
        Json outputs = Json::array();
        bool foundOutput = false;
        for (const auto &[tag, bytes] : parts)
            if (tag == 0x4e47534f || tag == 0x3547534f) {
                if (foundOutput)
                    fail("Conflicting checkpoint output signatures");
                foundOutput = true;
                outputs = dxbc_detail::signature(bytes, tag == 0x3547534f);
            }
        const auto masks = dxbc_detail::occupied(outputs, parsed.code, 2);
        for (uint32_t i = 0; i < masks.size(); ++i)
            if (masks[i])
                no = i + 1;
        if (nt > 4096)
            fail("Checkpoint exceeds temporary register limits");
        shadow = nt + no;
        declaredInputs();
        for (uint32_t i = 0; i < nt; ++i)
            registerSlots.push_back(
                {{"name", "r" + std::to_string(i)}, {"kind", "temporary"}, {"register", i}});
        for (uint32_t i = 0; i < no; ++i)
            registerSlots.push_back({{"name", "o" + std::to_string(i)}, {"kind", "output"}, {"register", i}});
        for (const auto &input : inputSlots)
            registerSlots.push_back(input);
        auto sortedArrays = arrays;
        std::sort(sortedArrays.begin(), sortedArrays.end(),
                  [](const auto &a, const auto &b) { return a.array < b.array; });
        for (const auto &a : sortedArrays)
            for (uint32_t element = 0; element < a.elements; ++element)
                registerSlots.push_back(
                    {{"name", "x" + std::to_string(a.array) + "[" + std::to_string(element) + "]"},
                     {"kind", "indexable_temporary"},
                     {"array", a.array},
                     {"element", element},
                     {"mask", a.mask}});
        if (!options.inputSelector.is_null())
            selector = validateSelector(options.inputSelector, raw, options.stage, inputSlots, known, phase);
        if (!phase.is_null())
            for (const auto &[name, c] : inputKeys(inputSlots, known))
                runtimeTerms.push_back(
                    {{"name", name}, {"component", c}, {"offset", 36 + 4 * runtimeTerms.size()}});
        headerBytes = !selector.is_null() || !phase.is_null() ? 32 : 16;
        if (!phase.is_null())
            headerBytes = uint32_t((36 + 4 * runtimeTerms.size() + 15) / 16 * 16);
        filtering = !selector.is_null() || !phase.is_null();
        stride = 32 + uint32_t(registerSlots.size()) * 32;
        total = headerBytes + uint64_t(options.capacity) * stride;
        const auto indexRegs = uint32_t((indexSlots + 3) / 4);
        added = shadow * 2 + 3 + (cacheInputs ? 1 + inputCount : 0) + hasCalls + !arrays.empty() + indexRegs +
                2 * !staged.empty() + filtering + 2 * !phase.is_null();
        if (nt + added + arrayElements * 3 > 4096 || total > 256 * 1024 * 1024)
            fail("Checkpoint exceeds temporary register or byte limits");
        if (temp)
            result[*temp][1] += added;
        else
            emit(104, {{added}});
        for (const auto &a : arrays)
            for (auto id : privateArrays.at(a.array))
                emit(105, {{id, a.elements, 4}});
        values = nt;
        flags = nt + shadow;
        control = nt + 2 * shadow;
        ticket = control + 1;
        address = control + 2;
        if (phase.is_null())
            emit(157, {{0x11e000, options.slot}});
        emit(180, {dst(control, 1), {0x11e000, options.slot}, imm(8), imm(1)});
        append(result, wrap(control, 0, 12));
        emit(54, {dst(control, 2), imm(0)});
        if (!phase.is_null()) {
            emit(165, {dst(control, 4), imm(24), {0x11e006, options.slot}});
            emit(165, {dst(control, 8), imm(28), {0x11e006, options.slot}});
        }
        for (uint32_t reg = values; reg < flags + shadow; ++reg)
            emit(54, {dst(reg), imm(0)});
        inputCache = address + 1;
        callDepth = inputCache + (cacheInputs ? 1 + inputCount : 0);
        indexCache = callDepth + hasCalls;
        safeIndices = indexCache + !arrays.empty();
        pairResults = safeIndices + indexRegs;
        selection = pairResults + 2 * !staged.empty();
        outputIndex = selection + filtering;
        outputValue = outputIndex + 1;
        if (hasCalls)
            emit(54, {dst(callDepth), imm(0)});
        for (const auto &a : arrays)
            for (auto id : privateArrays.at(a.array))
                for (uint32_t element = 0; element < a.elements; ++element)
                    emit(54, {indexableOperand(id, element), imm(0)});
        if (cacheInputs) {
            for (auto &[offset, operand] : identities) {
                emit(54, {dst(inputCache, 1u << ((offset - 8) / 4)), operand});
                operand = src(inputCache, (offset - 8) / 4);
            }
            for (uint32_t i = 0; i < inputCount; ++i) {
                emit(54, {dst(inputCache + 1 + i), imm(0)});
                emit(54, {dst(inputCache + 1 + i, inputSlots[i].at("mask")), inputOperand(inputSlots[i])});
            }
        }
        if (filtering) {
            std::map<InputKey, Words> cached;
            for (uint32_t i = 0; i < inputCount; ++i)
                for (uint32_t c = 0; c < 4; ++c)
                    if (inputSlots[i].at("mask").get<uint32_t>() & (1u << c))
                        cached[{inputSlots[i].at("name").get<std::string>(), c}] = src(inputCache + 1 + i, c);
            if (known.contains("primitive"))
                cached[{"vPrim", 0}] = src(inputCache, 0);
            if (known.contains("gs_instance"))
                cached[{"vGSInstanceID", 0}] = src(inputCache, 1);
            emit(54, {dst(selection, 1), imm(UINT32_MAX)});
            if (!phase.is_null()) {
                emit(165, {dst(selection, 4), imm(32), {0x11e006, options.slot}});
                emit(ifNonzero, {src(selection, 2)});
            }
            for (const auto &term : !phase.is_null() ? runtimeTerms : selector.at("inputs")) {
                if (!phase.is_null())
                    emit(165, {dst(selection, 2), imm(term.at("offset")), {0x11e006, options.slot}});
                emit(32, {dst(selection, 2), cached.at({term.at("name"), term.at("component")}),
                          !phase.is_null() ? src(selection, 1) : imm(term.at("bits"))});
                emit(1, {dst(selection, 1), src(selection, 0), src(selection, 1)});
            }
            if (!phase.is_null())
                emit(21);
            emit(ifNonzero, {src(selection, 0)});
            emit(180, {dst(ticket, 1), {0x11e000, options.slot}, imm(16), imm(1)});
            append(result, wrap(ticket, 0, 20));
            emit(21);
        }
    }
    Rows snapshot(const Words &tokenOperand, const Words &opcodeOperand) const {
        Rows body{op(180, {dst(ticket, 1), {0x11e000, options.slot}, imm(0), imm(1)})};
        append(body, wrap(ticket, 0, 4));
        body.push_back(op(79, {dst(ticket, 2), src(ticket, 0),
                               !phase.is_null() ? src(control, 3) : imm(options.capacity)}));
        body.push_back(op(35, {dst(address, 1), src(ticket, 0), imm(stride), imm(headerBytes)}));
        body.push_back(op(ifNonzero, {src(ticket, 1)}));
        std::vector<std::pair<uint32_t, Words>> fields{{0, src(control, 0)}, {4, src(control, 1)}};
        fields.insert(fields.end(), identities.begin(), identities.end());
        fields.emplace_back(16, tokenOperand);
        fields.emplace_back(20, opcodeOperand);
        if (hasCalls)
            fields.emplace_back(24, src(callDepth, 0));
        for (const auto &[offset, operand] : fields)
            store(body, offset, operand);
        const auto n = uint32_t(registerSlots.size());
        for (uint32_t i = 0; i < n; ++i) {
            const auto &record = registerSlots[i];
            const bool array = record.at("kind") == "indexable_temporary";
            if (trace || !phase.is_null()) {
                Words operand, valid;
                if (array) {
                    const auto ids = privateArrays.at(record.at("array"));
                    operand = indexableOperand(ids[0], record.at("element"), 15, 0xe4);
                    valid = indexableOperand(ids[1], record.at("element"), 15, 0xe4);
                } else {
                    operand = src(i < shadow ? values + i : inputCache + 1 + i - shadow);
                    if (i < shadow)
                        valid = src(flags + i);
                    else {
                        valid = {0x4002};
                        for (uint32_t c = 0; c < 4; ++c)
                            valid.push_back((record.at("mask").get<uint32_t>() >> c) & 1);
                    }
                }
                store(body, 32 + i * 16, operand, true);
                store(body, 32 + n * 16 + i * 16, valid, true);
                continue;
            }
            for (uint32_t c = 0; c < 4; ++c) {
                Words operand, valid;
                if (i < shadow) {
                    operand = src(values + i, c);
                    valid = src(flags + i, c);
                } else if (array) {
                    const auto ids = privateArrays.at(record.at("array"));
                    operand = indexableOperand(ids[0], record.at("element"), 15, {}, c);
                    valid = indexableOperand(ids[1], record.at("element"), 15, {}, c);
                } else if (record.at("mask").get<uint32_t>() & (1u << c)) {
                    operand = cacheInputs ? src(inputCache + 1 + i - shadow, c) : inputOperand(record, c);
                    valid = imm(1);
                } else {
                    operand = imm(0);
                    valid = imm(0);
                }
                store(body, 32 + i * 16 + c * 4, operand);
                store(body, 32 + n * 16 + i * 16 + c * 4, valid);
            }
        }
        body.push_back(op(21));
        body.push_back(op(30, {dst(control, 2), src(control, 1), imm(1)}));
        if (!phase.is_null()) {
            Rows guarded{op(32, {dst(ticket, 4), src(control, 2), tokenOperand}),
                         op(32, {dst(ticket, 8), src(control, 2), imm(UINT32_MAX)}),
                         op(60, {dst(ticket, 4), src(ticket, 2), src(ticket, 3)}),
                         op(ifNonzero, {src(ticket, 2)})};
            append(guarded, body);
            guarded.push_back(op(21));
            body = std::move(guarded);
        }
        if (filtering) {
            body.insert(body.begin(), op(ifNonzero, {src(selection, 0)}));
            body.push_back(op(21));
        }
        return body;
    }
    void execute(const Words &originalRow, bool track = true) {
        const auto opcode = originalRow[0] & 2047;
        const auto parsedOperands = instructionOperands(originalRow);
        const auto &operands = parsedOperands.ranges;
        std::vector<Words> rewritten;
        for (const auto &[a, b] : operands)
            rewritten.push_back(slice(originalRow, a, b));
        if (!arrays.empty()) {
            uint32_t usedIndices = 0;
            for (auto &words : rewritten)
                words = rewriteIndices(words, [&](const ArrayAccess &access) {
                    const auto reg = safeIndices + usedIndices / 4, c = usedIndices % 4;
                    ++usedIndices;
                    const auto target = dst(reg, 1u << c), value = src(reg, c);
                    if (access.relative)
                        emit(30, {target, *access.relative, imm(access.offset)});
                    else
                        emit(54, {target, imm(access.offset)});
                    // Clamp before RET: speculative lowering can still issue the original access.
                    emit(80, {dst(indexCache, 8), value, imm(arraySizes.at(access.array))});
                    emit(55, {target, src(indexCache, 3), imm(0), value});
                    emit(ifNonzero, {src(indexCache, 3)});
                    emit(166, {dst(options.slot, 1, 30), imm(4), imm(2)});
                    emit(62);
                    emit(21);
                    return value;
                });
        }
        auto instruction = [&] {
            auto row = parsedOperands.prefix;
            for (const auto &words : rewritten)
                row.insert(row.end(), words.begin(), words.end());
            if (row.size() > 127)
                fail("Rewritten checkpoint instruction length exceeds DXBC limit");
            row[0] = (row[0] & ~0x7f000000u) | (uint32_t(row.size()) << 24);
            return row;
        };
        if (!track) {
            result.push_back(instruction());
            return;
        }
        if (opcode == 4 || opcode == 5) {
            // Test CALLC at entry; its callee may change the condition register.
            if (opcode == 5)
                emit(31 | (originalRow[0] & (1u << 18)), {rewritten[0]});
            emit(79, {dst(callDepth, 2), src(callDepth, 0), imm(32)});
            emit(ifNonzero, {src(callDepth, 1)});
            emit(30, {dst(callDepth, 1), src(callDepth, 0), imm(1)});
            result.push_back(instruction());
            emit(30, {dst(callDepth, 1), src(callDepth, 0), imm(UINT32_MAX)});
            emit(21);
            if (opcode == 5)
                emit(21);
            return;
        }
        if (auto found = emissions.find(opcode); found != emissions.end()) {
            result.push_back(instruction());
            if (found->second == 1 || found->second == 3)
                for (uint32_t reg = nt; reg < shadow; ++reg) {
                    emit(54, {dst(values + reg), imm(0)});
                    emit(54, {dst(flags + reg), imm(0)});
                }
            return;
        }
        const uint32_t nd = noDestination.contains(opcode) || operands.empty() ? 0
                            : twoDestinations.contains(opcode)                 ? 2
                                                                               : 1;
        if (!phase.is_null() && nd) {
            std::vector<size_t> dynamic;
            for (uint32_t d = 0; d < nd; ++d) {
                const auto t = originalRow[operands.at(d).first];
                if (((t >> 12) & 255) == 2 && ((t >> 22) & 7))
                    dynamic.push_back(d);
            }
            if (!dynamic.empty()) {
                if (nd != 1 || dynamic != std::vector<size_t>{0})
                    fail("HS relative output requires a single destination");
                const auto t = originalRow[operands[0].first], mask = (t >> 4) & 15, repr = (t >> 22) & 7;
                if ((t >> 31) || (t & 3) != 2 || ((t >> 2) & 3) || ((t >> 20) & 3) != 1 || !mask ||
                    (repr != 2 && repr != 3))
                    fail("Unsupported HS relative output destination");
                const auto relative = slice(rewritten[0], repr == 2 ? 1 : 2, rewritten[0].size());
                const auto offset = repr == 2 ? 0 : rewritten[0][1];
                emit(30, {dst(outputIndex, 1), relative, imm(offset)});
                emit(80, {dst(outputIndex, 2), src(outputIndex, 0), imm(no)});
                emit(ifNonzero, {src(outputIndex, 1)});
                emit(166, {dst(options.slot, 1, 30), imm(4), imm(4)});
                emit(62);
                emit(21);
                rewritten[0] = dst(outputValue, mask);
                result.push_back(instruction());
                auto target = Words{dst(0, mask, 2)[0] | (2u << 22)};
                const auto index = src(outputIndex, 0);
                target.insert(target.end(), index.begin(), index.end());
                emit(54, {target, {0x100006 | (swizzle(mask) << 4), outputValue}});
                for (uint32_t reg = 0; reg < no; ++reg) {
                    emit(32, {dst(outputIndex, 2), src(outputIndex, 0), imm(reg)});
                    emit(ifNonzero, {src(outputIndex, 1)});
                    emit(54, {dst(values + nt + reg, mask), src(outputValue)});
                    emit(54, {dst(flags + nt + reg, mask), imm(1)});
                    emit(21);
                }
                return;
            }
        }
        Rows before, after;
        for (uint32_t destination = 0; destination < nd; ++destination) {
            const auto [a, b] = operands.at(destination);
            const auto t = originalRow[a], kind = (t >> 12) & 255;
            if (kind == 13)
                continue;
            if (kind == 3) {
                const auto d = indexableDestination(slice(originalRow, a, b), arrays);
                const auto &access = d.access;
                // Capture both effective addresses before either original result can overwrite them.
                if (access.relative)
                    before.push_back(
                        op(30, {dst(indexCache, 1u << destination), *access.relative, imm(access.offset)}));
                const auto index =
                    access.relative ? std::optional<Words>(src(indexCache, destination)) : std::nullopt;
                const auto ids = privateArrays.at(access.array);
                after.push_back(
                    op(54, {indexableOperand(ids[0], access.offset, d.mask, {}, {}, index),
                            indexableOperand(access.array, access.offset, 15, swizzle(d.mask), {}, index)}));
                after.push_back(
                    op(54, {indexableOperand(ids[1], access.offset, d.mask, {}, {}, index), imm(1)}));
                continue;
            }
            if (kind != 0 && kind != 2)
                fail("Unsupported checkpoint destination kind");
            const auto mask = (t >> 4) & 15;
            if (b - a != 2 || (t >> 31) || (t & 3) != 2 || ((t >> 2) & 3) || ((t >> 20) & 3) != 1 ||
                ((t >> 22) & 7) || !mask)
                fail("Checkpoint requires static masked destinations");
            const auto reg = originalRow[a + 1], target = kind == 0 ? reg : nt + reg;
            if (reg >= (kind == 0 ? nt : no))
                fail("Checkpoint destination outside declaration");
            if (kind == 2) {
                rewritten[destination] = dst(values + target, mask);
                after.push_back(
                    op(54, {slice(originalRow, a, b), {0x100006 | (swizzle(mask) << 4), values + target}}));
            } else
                after.push_back(op(54, {dst(values + target, mask), {0x100006 | (swizzle(mask) << 4), reg}}));
            after.push_back(op(54, {dst(flags + target, mask), imm(1)}));
        }
        for (size_t i = nd; i < operands.size(); ++i)
            if (((originalRow[operands[i].first] >> 12) & 255) == 2)
                fail("Checkpoint cannot read original output operands");
        append(result, before);
        result.push_back(instruction());
        append(result, after);
    }

  public:
    CheckpointProgram(Bytes raw, const CheckpointOptions &opts)
        : options(opts), parsed(checkpoint::program(raw, opts.stage)), original(parsed.code.instructions),
          ops(original), catalog(parsed.catalog), split(parsed.split), trace(!opts.token) {
        for (const auto &[tag, bytes] : readDxbcParts(raw))
            parts.emplace_back(tag, std::vector<uint8_t>(bytes.begin(), bytes.end()));
        initialize(raw);
    }
    OutputLogShader run() {
        for (size_t index = split; index < ops.size(); ++index) {
            const auto &row = ops[index];
            const auto opcode = row[0] & 2047;
            if (selected.contains(index)) {
                if (shared) {
                    emit(54, {dst(control, 4), imm(catalog[index].at("token"))});
                    emit(54, {dst(control, 8), imm(opcode)});
                    emit(4, {{0x10a000, helperLabel}});
                } else
                    append(result, snapshot(imm(catalog[index].at("token")), imm(opcode)));
            }
            if (staged.contains(index)) {
                const auto args = instructionOperands(row);
                std::vector<Words> destinations;
                auto pair = args.prefix;
                for (uint32_t d = 0; d < 2; ++d) {
                    const auto [a, b] = args.ranges.at(d);
                    destinations.push_back(slice(row, a, b));
                    const auto temp = dst(pairResults + d, (row[a] >> 4) & 15);
                    pair.insert(pair.end(), temp.begin(), temp.end());
                }
                for (size_t i = 2; i < args.ranges.size(); ++i) {
                    const auto [a, b] = args.ranges[i];
                    pair.insert(pair.end(), row.begin() + a, row.begin() + b);
                }
                if (pair.size() > 127)
                    fail("Staged checkpoint instruction exceeds DXBC length limit");
                pair[0] = (pair[0] & ~0x7f000000u) | (uint32_t(pair.size()) << 24);
                execute(pair, false);
                for (uint32_t d = 0; d < 2; ++d)
                    execute(op(54, {destinations[d], src(pairResults + d)}));
            } else
                execute(row);
        }
        if (shared) {
            emit(44, {{0x10a000, helperLabel}});
            append(result, snapshot(src(control, 2), src(control, 3)));
            emit(62);
        }
        if (!phase.is_null()) {
            auto complete = sliceRows(original, 0, phase.at("start").get<size_t>() + 1);
            append(complete, result);
            append(complete, sliceRows(original, phase.at("end"), original.size()));
            complete.insert(complete.begin() + phases.at(0).at("start").get<size_t>(),
                            op(157, {{0x11e000, options.slot}}));
            result = std::move(complete);
        }
        auto patched = parsed.code;
        patched.header[0] = (patched.header[0] & ~255u) | 0x50;
        patched.instructions = std::move(result);
        std::erase_if(parts, [&](const auto &p) { return p.first == parsed.tag; });
        parts.emplace_back(SHEX, writeDxbcProgram(patched));
        auto feature =
            std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == SFI0; });
        if (feature == parts.end()) {
            parts.emplace_back(SFI0, std::vector<uint8_t>(8));
            feature = std::prev(parts.end());
        }
        if (feature->second.size() != 8)
            fail("Invalid checkpoint shader feature flags");
        const auto bits = Reader(feature->second).read<uint64_t>() | 4 | (options.slot >= 8 ? 8 : 0);
        std::memcpy(feature->second.data(), &bits, 8);
        const std::set<uint32_t> stale{0x54415453, 0x47424453, 0x42445053,
                                       0x42444c49, 0x49435253, 0x4e444c49};
        DxbcParts finalParts;
        for (const auto &[tag, bytes] : parts)
            if (!stale.contains(tag))
                finalParts.emplace_back(tag, bytes);
        Json meta{{"capacity", options.capacity},
                  {"record_stride", stride},
                  {"total_bytes", total},
                  {"registers", registerSlots.size()},
                  {"register_slots", registerSlots},
                  {"known_inputs", known},
                  {"signature", Json::array()},
                  {"checkpoint", trace ? Json(nullptr) : catalog.at(*token)},
                  {"checkpoint_timing", "before_original_instruction"},
                  {"private_slot", options.slot},
                  {"inputs_added", false},
                  {"outputs_added", false},
                  {"original_profile", parsed.profile},
                  {"helper_profile", options.stage + "_5_0"},
                  {"shader_stage", options.stage},
                  {"counter_wrap_checked", true},
                  {"value_offset", 32}};
        if (trace) {
            meta["trace"] = true;
            meta["checkpoints"] = Json::array();
            for (auto index : selected)
                meta["checkpoints"].push_back(catalog.at(index));
            meta["logging_strategy"] = shared ? "shared_private_subroutine" : "inline";
        }
        if (hasCalls) {
            meta["call_depth_offset"] = 24;
            meta["static_call_depth"] = calls.depth;
            meta["subroutine_labels"] = Json::array();
            for (const auto &[label, index] : calls.labels)
                meta["subroutine_labels"].push_back(label);
        }
        if (!arrays.empty()) {
            meta["indexable_temporaries"] = Json::array();
            for (const auto &a : arrays)
                meta["indexable_temporaries"].push_back({{"array", a.array},
                                                         {"elements", a.elements},
                                                         {"components", a.components},
                                                         {"mask", a.mask}});
            meta["temporary_registers"] = nt + added + arrayElements * 3;
        }
        if (!staged.empty()) {
            meta["staged_result_tokens"] = staged;
            meta["result_writeback_order"] = "first_then_second";
        }
        if (!phase.is_null()) {
            meta["hs_phase"] = phase;
            meta["runtime_input_filter_offset"] = 32;
            meta["runtime_input_terms"] = runtimeTerms;
            meta["runtime_checkpoint_token"] = trace ? Json(UINT32_MAX) : catalog.at(*token).at("token");
            meta["logging_selection"] = "runtime_token_same_instrumented_program";
            meta["arithmetic_refactoring_allowed"] =
                std::any_of(original.begin(), original.end(),
                            [](const auto &r) { return (r[0] & 2047) == 106 && (r[0] & (1u << 11)); });
        }
        if (filtering)
            meta["data_offset"] = headerBytes;
        if (!domain.empty())
            meta["domain"] = domain;
        if (!selector.is_null()) {
            meta["input_selector"] = selector;
            meta["matched_count_offset"] = 16;
            meta["matched_overflow_offset"] = 20;
        }
        return {makeDxbc(finalParts), std::move(meta)};
    }
};
} // namespace
OutputLogShader instrumentCheckpoint(Bytes original, const CheckpointOptions &options) {
    return CheckpointProgram(original, options).run();
}
} // namespace flora
