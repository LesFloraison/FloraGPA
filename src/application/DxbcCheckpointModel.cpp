#include "DxbcCheckpointModel.h"
#include "DxbcOperandArities.h"
#include <algorithm>
#include <limits>

namespace flora::checkpoint {
namespace {
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
size_t endOperand(const Words &words, size_t start) {
    std::set<unsigned> unused;
    return dxbc_detail::operand(words, start, unused, 0, false);
}
void completeOperand(const Words &words) {
    if (endOperand(words, 0) != words.size())
        fail("Unexpected trailing operand words");
}
Words slice(const Words &words, size_t begin, size_t end) {
    if (begin > end || end > words.size())
        fail("Checkpoint operand bounds");
    return {words.begin() + begin, words.begin() + end};
}
const ArrayDeclaration &declaration(const Arrays &arrays, uint32_t id) {
    auto found = std::find_if(arrays.begin(), arrays.end(), [id](const auto &a) { return a.array == id; });
    if (found == arrays.end())
        fail("Undeclared indexable temporary");
    return *found;
}
struct Dimension {
    Words constant;
    std::optional<Words> relative;
    unsigned mode;
    uint64_t offset() const {
        return constant.empty()
                   ? 0
                   : uint64_t(constant[0]) | (constant.size() == 2 ? uint64_t(constant[1]) << 32 : 0);
    }
};
struct Address {
    Words prefix;
    std::vector<Dimension> dimensions;
    Words suffix;
};
Address address(const Words &words) {
    completeOperand(words);
    size_t pos = 1;
    for (auto extended = words[0] >> 31; extended;)
        extended = words.at(pos++) >> 31;
    Address result{slice(words, 0, pos), {}, {}};
    for (unsigned i = 0; i < ((words[0] >> 20) & 3); ++i) {
        Dimension d{{}, {}, (words[0] >> (22 + i * 3)) & 7};
        const auto start = pos;
        if (d.mode == 0 || d.mode == 3)
            ++pos;
        else if (d.mode == 1 || d.mode == 4)
            pos += 2;
        else if (d.mode != 2)
            fail("Unsupported operand indexing");
        d.constant = slice(words, start, pos);
        if (d.mode >= 2 && d.mode <= 4) {
            auto end = endOperand(words, pos);
            d.relative = slice(words, pos, end);
            pos = end;
        }
        result.dimensions.push_back(std::move(d));
    }
    result.suffix = slice(words, pos, words.size());
    return result;
}
ArrayAccess arrayAccess(const Address &a) {
    if (a.dimensions.size() != 2 || a.dimensions[0].mode != 0 ||
        (a.dimensions[1].mode != 0 && a.dimensions[1].mode != 2 && a.dimensions[1].mode != 3))
        fail("Unsupported indexable temporary addressing");
    return {uint32_t(a.dimensions[0].offset()), uint32_t(a.dimensions[1].offset()), a.dimensions[1].relative};
}
bool declarationOpcode(uint32_t opcode) {
    return (opcode >= 88 && opcode <= 106) || (opcode >= 143 && opcode <= 162) || opcode == 53 ||
           opcode == 206;
}
uint32_t countDeclaration(const Rows &rows, uint32_t opcode, uint32_t mask, uint32_t maximum) {
    std::optional<uint32_t> value;
    for (const auto &row : rows)
        if ((row.at(0) & 2047) == opcode) {
            auto next = (row[0] >> 11) & mask;
            if (value || !next || next > maximum)
                fail("Invalid checkpoint input count or domain");
            value = next;
        }
    if (!value)
        fail("Missing checkpoint input count or domain");
    return *value;
}
void addInput(DeclaredInputs &inputs, Json record, unsigned mask) {
    if (record.value("register", 0u) >= 32)
        fail("Input register exceeds signature bounds");
    for (auto &existing : inputs.inputSlots)
        if (existing.at("name") == record.at("name")) {
            existing["mask"] = existing.at("mask").get<uint32_t>() | mask;
            return;
        }
    record["mask"] = mask;
    inputs.inputSlots.push_back(std::move(record));
}
void primitive(DeclaredInputs &result) {
    result.known["primitive"] = {{"operand", "vPrim"}, {"offset", 8}};
    result.identity.push_back(Json::array({8, Json::array({0xb001})}));
}
uint32_t swizzle(uint32_t mask) {
    if (!mask || mask > 15)
        fail("Invalid input component mask");
    unsigned first = 0, value = 0;
    while (!(mask & (1u << first)))
        ++first;
    for (unsigned c = 0; c < 4; ++c)
        value |= ((mask & (1u << c)) ? c : first) << (c * 2);
    return value;
}
} // namespace

Arrays indexableDeclarations(const Rows &rows) {
    Arrays result;
    for (const auto &row : rows) {
        if ((row.at(0) & 2047) != 105)
            continue;
        if (row.size() != 4 || !row[2] || row[2] > 4096 || !row[3] || row[3] > 4 ||
            std::any_of(result.begin(), result.end(), [&](const auto &a) { return a.array == row[1]; }))
            fail("Invalid indexable temporary declaration");
        result.push_back({row[1], row[2], row[3], (1u << row[3]) - 1});
    }
    return result;
}
ArrayDestination indexableDestination(const Words &words, const Arrays &arrays) {
    const auto a = address(words);
    const auto token = words[0], mask = (token >> 4) & 15;
    if ((token >> 31) || ((token >> 12) & 255) != 3 || (token & 3) != 2 || ((token >> 2) & 3) ||
        ((token >> 20) & 3) != 2 || ((token >> 22) & 7) || !mask)
        fail("Unsupported indexable temporary destination");
    auto access = arrayAccess(a);
    if (mask & ~declaration(arrays, access.array).mask)
        fail("Indexable destination outside its declaration");
    return {std::move(access), mask};
}
std::vector<ArrayAccess> indexableAccesses(const Words &words, const Arrays &arrays) {
    // Validate the complete nested operand once before recursion, including the depth bound.
    completeOperand(words);
    std::vector<ArrayAccess> result;
    std::function<void(const Words &)> visit = [&](const Words &current) {
        auto a = address(current);
        for (const auto &d : a.dimensions)
            if (d.relative)
                visit(*d.relative);
        if (((current[0] >> 12) & 255) != 3)
            return;
        auto access = arrayAccess(a);
        const auto &decl = declaration(arrays, access.array);
        if (!access.relative && access.offset >= decl.elements)
            fail("Indexable temporary static access is out of bounds");
        result.push_back(std::move(access));
    };
    visit(words);
    return result;
}
bool changesIndex(const Words &destination, const std::optional<Words> &relative) {
    if (!relative)
        return false;
    completeOperand(destination);
    completeOperand(*relative);
    const auto dt = destination[0], rt = relative->at(0), kind = (rt >> 12) & 255;
    if (((dt >> 12) & 255) != kind || !(((dt >> 4) & 15) & (1u << ((rt >> 4) & 3))))
        return false;
    if (kind == 0)
        return destination.at(1) == relative->at(1);
    if (kind == 3)
        return destination.at(1) == relative->at(1) &&
               (((dt >> 25) & 7) || ((rt >> 25) & 7) || destination.at(2) == relative->at(2));
    return false;
}
Words rewriteIndices(const Words &words, const std::function<Words(const ArrayAccess &)> &replace) {
    completeOperand(words);
    std::function<Words(const Words &)> visit = [&](const Words &current) {
        auto a = address(current);
        for (auto &d : a.dimensions)
            if (d.relative)
                d.relative = visit(*d.relative);
        Words result = a.prefix;
        if (((current[0] >> 12) & 255) == 3) {
            const auto access = arrayAccess(a);
            auto safe = replace(access);
            completeOperand(safe);
            result[0] = (result[0] & ~(7u << 25)) | (2u << 25);
            result.push_back(access.array);
            result.insert(result.end(), safe.begin(), safe.end());
        } else {
            for (const auto &d : a.dimensions) {
                result.insert(result.end(), d.constant.begin(), d.constant.end());
                if (d.relative)
                    result.insert(result.end(), d.relative->begin(), d.relative->end());
            }
            result.insert(result.end(), a.suffix.begin(), a.suffix.end());
        }
        return result;
    };
    return visit(words);
}
Words indexableOperand(uint32_t array, uint32_t index, uint32_t mask, std::optional<uint32_t> swiz,
                       std::optional<uint32_t> component, const std::optional<Words> &relative) {
    if (!mask || mask > 15 || (swiz && *swiz > 255) || (component && *component > 3))
        fail("Invalid indexable operand components");
    uint32_t token = 0x203000 | (component ? 10 | (*component << 4)
                                 : swiz    ? 6 | (*swiz << 4)
                                           : 2 | (mask << 4));
    if (!relative)
        return {token, array, index};
    completeOperand(*relative);
    Words result{token | (2u << 25), array};
    result.insert(result.end(), relative->begin(), relative->end());
    return result;
}
Json hullPhases(const Rows &rows) {
    std::vector<size_t> starts;
    for (size_t i = 0; i < rows.size(); ++i)
        if (auto op = rows[i].at(0) & 2047; op >= 114 && op <= 116)
            starts.push_back(i);
    if (starts.empty())
        fail("Hull shader has no original executable phase");
    Json phases = Json::array();
    for (size_t i = 0; i < starts.size(); ++i) {
        const auto start = starts[i], end = i + 1 < starts.size() ? starts[i + 1] : rows.size();
        auto split = start + 1;
        while (split < end && declarationOpcode(rows[split].at(0) & 2047))
            ++split;
        if (split == end || (rows[end - 1].at(0) & 2047) != 62)
            fail("Hull phase must end in RET");
        const char *kind = (rows[start][0] & 2047) == 114   ? "control_points"
                           : (rows[start][0] & 2047) == 115 ? "fork"
                                                            : "join";
        phases.push_back({{"id", i}, {"kind", kind}, {"start", start}, {"end", end}, {"split", split}});
    }
    return phases;
}
DeclaredInputs hullInputs(const Rows &globals, const Rows &rows, const Json &phase) {
    const auto inputs = countDeclaration(globals, 147, 63, 32),
               outputs = countDeclaration(globals, 148, 63, 32);
    const auto phaseKind = phase.at("kind").get<std::string>();
    const auto special = phaseKind == "control_points" ? 22u
                         : phaseKind == "fork"         ? 23u
                         : phaseKind == "join"         ? 24u
                                                       : 0u;
    if (!special)
        fail("Invalid hull phase kind");
    const char *name = special == 22   ? "vOutputControlPointID"
                       : special == 23 ? "vForkInstanceID"
                                       : "vJoinInstanceID";
    DeclaredInputs result;
    for (const auto &row : rows) {
        const auto op = row.at(0) & 2047;
        if (op != 95 && op != 96 && op != 97)
            continue;
        const auto token = row.at(1), kind = (token >> 12) & 255, dimensions = (token >> 20) & 3,
                   mask = (token >> 4) & 15;
        const size_t tail = op == 95 ? 0 : 1;
        if (kind == 11 && row.size() == 2 && token == 0xb000) {
            primitive(result);
            continue;
        }
        if (kind == special && row.size() == 2 && token == (kind << 12)) {
            addInput(result,
                     {{"name", name},
                      {"kind", "phase_instance"},
                      {"operand_kind", kind},
                      {"indices", Json::array()},
                      {"scalar", true}},
                     1);
            result.known["hs_instance"] = {{"operand", name}, {"offset", 12}};
            result.identity.push_back(Json::array({12, Json::array({(kind << 12) | 1})}));
            continue;
        }
        if ((token >> 31) || ((token >> 22) & 63) || (token & 3) != 2 || ((token >> 2) & 3) || !mask)
            fail("Unsupported HS input declaration");
        if ((special == 22 ? kind == 1 : kind == 25 || kind == 26) && dimensions == 2 &&
            row.size() == 4 + tail && row[2] && row[2] <= (kind == 26 ? outputs : inputs)) {
            const std::string prefix = kind == 1 ? "v" : kind == 25 ? "vicp" : "vocp";
            for (uint32_t cp = 0; cp < row[2]; ++cp)
                addInput(result,
                         {{"name", prefix + "[" + std::to_string(cp) + "][" + std::to_string(row[3]) + "]"},
                          {"kind", kind == 26 ? "output_control_point" : "input_control_point"},
                          {"control_point", cp},
                          {"register", row[3]},
                          {"operand_kind", kind},
                          {"indices", Json::array({cp, row[3]})}},
                         mask);
        } else if (kind == 27 && special == 24 && dimensions == 1 && row.size() == 3 + tail)
            addInput(result,
                     {{"name", "vpc" + std::to_string(row[2])},
                      {"kind", "patch_constant"},
                      {"register", row[2]},
                      {"operand_kind", 27},
                      {"indices", Json::array({row[2]})}},
                     mask);
        else
            fail("Unsupported HS phase input register");
    }
    return result;
}
DeclaredInputs domainInputs(const Rows &rows) {
    const auto domain = countDeclaration(rows, 149, 3, 3), count = countDeclaration(rows, 147, 63, 32);
    DeclaredInputs result;
    result.domain = domain == 1 ? "isoline" : domain == 2 ? "tri" : "quad";
    for (const auto &row : rows) {
        const auto op = row.at(0) & 2047;
        if (op != 95 && op != 96 && op != 97)
            continue;
        const auto token = row.at(1), kind = (token >> 12) & 255, dimensions = (token >> 20) & 3,
                   mask = (token >> 4) & 15;
        const size_t tail = op == 95 ? 0 : 1;
        if (kind == 11 && op == 95 && row.size() == 2 && token == 0xb000) {
            primitive(result);
            continue;
        }
        if ((token >> 31) || ((token >> 22) & 63) || (token & 3) != 2 || ((token >> 2) & 3) || !mask)
            fail("Unsupported DS input declaration");
        if (kind == 25 && dimensions == 2 && row.size() == 4 + tail && row[2] == count) {
            for (uint32_t cp = 0; cp < count; ++cp)
                addInput(result,
                         {{"name", "vicp[" + std::to_string(cp) + "][" + std::to_string(row[3]) + "]"},
                          {"kind", "input_control_point"},
                          {"control_point", cp},
                          {"register", row[3]},
                          {"operand_kind", 25},
                          {"indices", Json::array({cp, row[3]})}},
                         mask);
        } else if (kind == 27 && dimensions == 1 && row.size() == 3 + tail)
            addInput(result,
                     {{"name", "vpc" + std::to_string(row[2])},
                      {"kind", "patch_constant"},
                      {"register", row[2]},
                      {"operand_kind", 27},
                      {"indices", Json::array({row[2]})}},
                     mask);
        else if (kind == 28 && dimensions == 0 && row.size() == 2 && op == 95 &&
                 !(mask & ~(domain == 2 ? 7u : 3u))) {
            addInput(result,
                     {{"name", "vDomain"},
                      {"kind", "domain_location"},
                      {"operand_kind", 28},
                      {"indices", Json::array()}},
                     mask);
            Json components = Json::array();
            for (unsigned c = 0; c < 3; ++c)
                if (mask & (1u << c))
                    components.push_back(c);
            result.known["domain"] = {{"operand", "vDomain"}, {"components", components}};
        } else
            fail("Unsupported DS input register kind or dimensions");
    }
    return result;
}
Words inputOperand(const Json &record, std::optional<uint32_t> component) {
    if (component && *component > 3)
        fail("Invalid input component");
    if (record.value("scalar", false))
        return {(record.at("operand_kind").get<uint32_t>() << 12) | 1};
    const auto selection = component ? 10 | (*component << 4) : 6 | (swizzle(record.at("mask")) << 4);
    if (!record.contains("operand_kind"))
        return {0x201000 | selection, record.at("vertex"), record.at("register")};
    const auto indices = record.at("indices").get<Words>();
    if (indices.size() > 3)
        fail("Invalid input dimensions");
    Words result{(uint32_t(indices.size()) << 20) | (record.at("operand_kind").get<uint32_t>() << 12) |
                 selection};
    result.insert(result.end(), indices.begin(), indices.end());
    return result;
}
InstructionOperands instructionOperands(const Words &row) {
    const auto opcode = row.at(0) & 2047;
    const auto count = dxbc_detail::operandArities.find(opcode);
    if (count == dxbc_detail::operandArities.end())
        fail("Unsupported shader checkpoint opcode");
    size_t pos = 1;
    for (auto extended = row[0] >> 31; extended;)
        extended = row.at(pos++) >> 31;
    InstructionOperands result{slice(row, 0, pos), {}};
    for (unsigned i = 0; i < count->second; ++i) {
        auto end = endOperand(row, pos);
        result.ranges.emplace_back(pos, end);
        pos = end;
    }
    if (pos != row.size())
        fail("Unsupported shader checkpoint instruction layout");
    return result;
}
bool dependentResult(const Words &row, const Arrays &arrays) {
    static const std::set<unsigned> two{38, 77, 78, 81, 132, 133, 142};
    if (!two.contains(row.at(0) & 2047))
        return false;
    auto args = instructionOperands(row);
    auto [a, b] = args.ranges.at(1);
    if (((row[a] >> 12) & 255) != 3)
        return false;
    auto relative = indexableDestination(slice(row, a, b), arrays).access.relative;
    auto [c, d] = args.ranges.at(0);
    return changesIndex(slice(row, c, d), relative);
}
CallGraph subroutines(const Rows &rows, size_t split) {
    if (split > rows.size())
        fail("Invalid subroutine declaration boundary");
    using Label = std::optional<uint32_t>;
    Label owner;
    CallGraph result;
    std::map<Label, std::set<uint32_t>> edges{{{}, {}}};
    for (size_t index = split; index < rows.size(); ++index) {
        const auto &row = rows[index];
        const auto op = row.at(0) & 2047;
        if (op == 44) {
            if (row.size() != 3 || row[1] != 0x10a000 || result.labels.contains(row[2]) || index == split ||
                (rows[index - 1].at(0) & 2047) != 62)
                fail("Invalid shader subroutine label");
            owner = row[2];
            result.labels[*owner] = index;
            edges[owner] = {};
        }
        result.owners[index] = owner;
        if (op == 4 || op == 5) {
            const auto start = op == 4 ? size_t(1) : endOperand(row, 1);
            if (start + 2 != row.size() || row.at(start) != 0x10a000)
                fail("Shader call requires a static label");
            result.targets[index] = row[start + 1];
            edges[owner].insert(row[start + 1]);
        }
    }
    for (const auto &[index, target] : result.targets)
        if (!result.labels.contains(target))
            fail("Shader call references an absent label");
    // Explicit DFS avoids consuming the C++ call stack on an untrusted label chain.
    std::map<Label, uint32_t> depths;
    std::set<Label> active;
    for (const auto &[root, unused] : edges) {
        if (depths.contains(root))
            continue;
        std::vector<std::pair<Label, bool>> work{{root, false}};
        while (!work.empty()) {
            auto [label, exit] = work.back();
            work.pop_back();
            if (exit) {
                uint32_t depth = 0;
                for (auto target : edges.at(label))
                    depth = std::max(depth, 1 + depths.at(Label(target)));
                depths[label] = depth;
                active.erase(label);
            } else {
                if (active.contains(label))
                    fail("Recursive shader calls are invalid");
                if (depths.contains(label))
                    continue;
                active.insert(label);
                work.emplace_back(label, true);
                for (auto target : edges.at(label))
                    work.emplace_back(Label(target), false);
            }
        }
    }
    result.depth = depths.at(Label{});
    return result;
}
Program program(Bytes raw, const std::string &stage) {
    const auto parts = readDxbcParts(raw);
    auto code = std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == 0x58454853; });
    if (code == parts.end())
        code = std::find_if(parts.begin(), parts.end(), [](const auto &p) { return p.first == 0x52444853; });
    if (code == parts.end())
        fail("Missing checkpoint shader program");
    Program result{readDxbcProgram(code->second), code->first, {}, 0, Json::array()};
    const auto version = result.code.header[0], kind = version >> 16, major = (version >> 4) & 15,
               minor = version & 15;
    if ((stage == "gs"   ? kind != 2 || !((major == 4 && minor <= 1) || (major == 5 && !minor))
         : stage == "ds" ? kind != 4 || major != 5 || minor
         : stage == "hs" ? kind != 3 || major != 5 || minor
                         : true))
        fail("Checkpoint profile does not match the selected shader stage");
    result.profile = stage + "_" + std::to_string(major) + "_" + std::to_string(minor);
    const auto &rows = result.code.instructions;
    while (result.split < rows.size() && declarationOpcode(rows[result.split].at(0) & 2047))
        ++result.split;
    const auto phases = stage == "hs" ? hullPhases(rows) : Json::array();
    static const std::set<unsigned> structural{6, 10, 18, 21, 22, 23, 44, 48};
    size_t offset = 2;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto opcode = rows[i][0] & 2047;
        Json entry{{"token", i},
                   {"word_offset", offset},
                   {"opcode", opcode},
                   {"words", rows[i]},
                   {"checkpoint_allowed", i >= result.split && !structural.contains(opcode)}};
        if (stage == "hs") {
            entry["checkpoint_allowed"] = false;
            for (const auto &phase : phases)
                if (i >= phase.at("start").get<size_t>() && i < phase.at("end").get<size_t>()) {
                    entry["hs_phase"] = phase.at("id");
                    entry["phase_kind"] = phase.at("kind");
                    entry["checkpoint_allowed"] =
                        i >= phase.at("split").get<size_t>() && !structural.contains(opcode);
                    break;
                }
        }
        result.catalog.push_back(std::move(entry));
        offset += rows[i].size();
    }
    return result;
}
} // namespace flora::checkpoint
