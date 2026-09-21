#include "SdbgVariables.h"
#include "DxbcCheckpointModel.h"
#include "ShaderDebugData.h"
#include "SourceVariables.h"
#include "SystemDisassembly.h"
#include <QRegularExpression>
#include <bit>

namespace flora {
namespace {
using Json = nlohmann::json;
using namespace checkpoint;
using namespace shader_debug;
constexpr uint32_t none = UINT32_MAX;
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
const std::set<uint32_t> noDest{2,   3,   4,   5,   6,   7,   8,   9,   10,  13,  18,  19,  20,  21,
                                22,  23,  31,  44,  48,  58,  62,  63,  76,  117, 118, 119, 120, 164,
                                166, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 190, 207, 208};
const std::set<uint32_t> twoDest{38, 77, 78, 81, 132, 133, 142},
    doubleDest{191, 192, 193, 194, 199, 200, 202};
const std::map<uint32_t, std::pair<std::string, uint32_t>> types{
    {1, {"bool", 4}}, {2, {"int", 4}}, {3, {"float", 4}}, {19, {"uint", 4}}, {39, {"double", 8}}};
struct Tables {
    Bytes base, ascii;
    uint32_t ints, strings;
    std::array<uint32_t, 21> h;
    std::map<std::string, Rows> tables;
    Json files, environment;
    explicit Tables(Bytes bytes) {
        const auto source = embedded({{"SDBG", bytes}}, true);
        if (source.at("status") == "invalid_embedded_source")
            throw std::runtime_error(source.at("issues").at(0).get<std::string>());
        files = source.at("files");
        environment = source.at("environment");
        h = Reader(bytes).array<uint32_t, 21>();
        base = sub(bytes, 84, bytes.size() - 84);
        ints = h[19];
        strings = h[20];
        ascii = sub(base, strings, base.size() - strings);
        const std::string names[]{"files",  "instructions", "variables",      "inputs",
                                  "tokens", "scopes",       "scope_variables"};
        const size_t widths[]{4, 99, 6, 6, 5, 5, 11};
        for (size_t i = 0; i < 7; ++i) {
            Reader r(sub(base, h[6 + 2 * i], size_t(h[5 + 2 * i]) * widths[i] * 4));
            for (uint32_t n = 0; n < h[5 + 2 * i]; ++n) {
                Words row;
                for (size_t c = 0; c < widths[i]; ++c)
                    row.push_back(r.read<uint32_t>());
                tables[names[i]].push_back(std::move(row));
            }
            if (!tables.contains(names[i]))
                tables[names[i]] = {};
        }
    }
    Words uints(uint32_t offset, uint32_t count) const {
        if (count > 65536 || offset > strings - ints || count > (strings - ints - offset) / 4)
            fail("SDBG uint list bounds");
        Reader r(sub(base, uint64_t(ints) + offset, uint64_t(count) * 4));
        Words result;
        for (uint32_t i = 0; i < count; ++i)
            result.push_back(r.read<uint32_t>());
        return result;
    }
    std::tuple<std::string, uint32_t, uint32_t> token(uint32_t index) const {
        const auto &tokens = tables.at("tokens");
        if (index >= tokens.size())
            fail("SDBG variable token bounds");
        const auto &t = tokens.at(index);
        const auto text = sub(ascii, t[4], t[3]);
        return {strictDebugText({reinterpret_cast<const char *>(text.data()), text.size()}), t[0], t[1]};
    }
};
struct Shape {
    std::vector<std::pair<std::string, uint64_t>> leaves;
    Json vectors = Json::array();
};
Shape shape(const Tables &s, const std::vector<Words> &scopeLists, uint32_t index,
            std::vector<uint32_t> parents = {}) {
    const auto &vars = s.tables.at("scope_variables"), &scopes = s.tables.at("scopes");
    if (index >= vars.size() || std::find(parents.begin(), parents.end(), index) != parents.end() ||
        parents.size() > 16)
        fail("Recursive SDBG variable shape");
    const auto &row = vars.at(index);
    s.token(row[0]);
    parents.push_back(index);
    const auto count = row[9];
    if (!count || count > 4096)
        fail("Unsupported SDBG component count");
    if (row[1] > 1 || row[2] > 3)
        fail("Opaque SDBG variable type");
    if (row[6] > 8)
        fail("SDBG array dimension limit");
    const auto dims = s.uints(row[7], row[6]), strides = s.uints(row[8], row[6]);
    for (const auto n : dims)
        if (!n || n > 4096)
            fail("SDBG array bounds");
    Shape result;
    if (row[5] != none) {
        if (row[5] >= scopes.size() || scopes[row[5]][0] != 3)
            fail("SDBG structure scope bounds");
        uint64_t offset = 0;
        for (const auto child : scopeLists.at(row[5])) {
            auto name = std::get<0>(s.token(vars.at(child)[0]));
            const auto last = name.rfind("::");
            if (last != std::string::npos)
                name = name.substr(last + 2);
            auto shaped = shape(s, scopeLists, child, parents);
            for (const auto &[suffix, at] : shaped.leaves)
                result.leaves.emplace_back("." + name + suffix, offset + at);
            for (auto v : shaped.vectors) {
                v["path"] = "." + name + v.at("path").get<std::string>();
                for (auto &m : v["members"])
                    m = "." + name + m.get<std::string>();
                result.vectors.push_back(v);
            }
            offset += vars.at(child)[9];
            if (result.leaves.size() > 4096)
                fail("SDBG structure component limit");
        }
    } else if (row[2] == 0)
        result.leaves.emplace_back("", 0);
    else if (row[2] == 1) {
        if (row[3] != 1 || row[4] < 1 || row[4] > 4)
            fail("SDBG vector shape");
        Json members = Json::array();
        for (uint32_t i = 0; i < row[4]; ++i) {
            const auto path = std::string(".") + "xyzw"[i];
            result.leaves.emplace_back(path, i);
            members.push_back(path);
        }
        result.vectors.push_back({{"path", ""}, {"members", members}});
    } else {
        if (row[3] < 1 || row[3] > 4 || row[4] < 1 || row[4] > 4)
            fail("SDBG matrix shape");
        for (uint32_t r = 0; r < row[3]; ++r) {
            Json members = Json::array();
            const auto prefix = "[" + std::to_string(r) + "]";
            for (uint32_t c = 0; c < row[4]; ++c) {
                const auto path = prefix + "[" + std::to_string(c) + "]";
                result.leaves.emplace_back(path, r * row[4] + c);
                members.push_back(path);
            }
            result.vectors.push_back({{"path", prefix}, {"members", members}});
        }
    }
    for (size_t d = dims.size(); d > 0; --d) {
        const auto n = dims[d - 1], stride = strides[d - 1];
        if (uint64_t(n) * result.leaves.size() > 4096)
            fail("SDBG expanded array limit");
        Shape expanded;
        for (uint32_t i = 0; i < n; ++i) {
            const auto prefix = "[" + std::to_string(i) + "]";
            for (const auto &[name, at] : result.leaves)
                expanded.leaves.emplace_back(prefix + name, uint64_t(i) * stride + at);
            for (auto v : result.vectors) {
                v["path"] = prefix + v.at("path").get<std::string>();
                for (auto &m : v["members"])
                    m = prefix + m.get<std::string>();
                expanded.vectors.push_back(v);
            }
        }
        result = std::move(expanded);
    }
    std::set<uint64_t> offsets;
    for (const auto &[name, at] : result.leaves)
        offsets.insert(at);
    if (result.leaves.size() != count || offsets.size() != count || *offsets.begin() != 0 ||
        *offsets.rbegin() != count - 1)
        fail("SDBG shape does not cover declared scalars");
    return result;
}
} // namespace
Json sdbgVariables(Bytes raw) {
    Json result{{"status", "unavailable"},    {"format", "SDBG assignments"}, {"shader_sha256", sha256(raw)},
                {"variables", Json::array()}, {"scopes", Json::array()},      {"issues", Json::array()}};
    try {
        const auto parts = shader_debug::chunks(raw);
        Tables s(parts.at("SDBG"));
        const auto target = s.environment.at("hlslTarget").get<std::string>(),
                   stage = target.substr(0, target.find('_'));
        if (stage != "gs" && stage != "ds" && stage != "hs") {
            result["issues"].push_back("SDBG assignment values require a supported native stage trace");
            return result;
        }
        result["shader_stage"] = stage;
        const auto &vars = s.tables.at("scope_variables"), &scalars = s.tables.at("variables"),
                   &scopes = s.tables.at("scopes");
        if (vars.size() > 4096 || scalars.size() > 65536 || scopes.size() > 4096)
            fail("SDBG variable inspection limit");
        std::map<uint32_t, std::map<uint32_t, uint32_t>> scalarByOwner;
        std::map<uint32_t, Words> ownerOrder;
        for (uint32_t i = 0; i < scalars.size(); ++i) {
            const auto &row = scalars[i];
            s.token(row[0]);
            if (row[4] >= vars.size() || row[5] >= vars[row[4]][9])
                fail("SDBG scalar owner bounds");
            if (!scalarByOwner[row[4]].emplace(row[5], i).second)
                fail("Duplicate SDBG scalar component identity");
            ownerOrder[row[4]].push_back(i);
        }
        std::vector<Words> scopeLists;
        size_t expanded = 0;
        for (const auto &row : scopes) {
            auto members = s.uints(row[4], row[3]);
            for (const auto v : members)
                if (v >= vars.size())
                    fail("SDBG scope member bounds");
            expanded += members.size();
            if (expanded > 1000000)
                fail("SDBG scope expansion limit");
            scopeLists.push_back(std::move(members));
        }
        for (uint32_t i = 0; i < vars.size(); ++i) {
            const auto &row = vars[i];
            auto [name, file, line] = s.token(row[0]);
            if (row[10] == none || row[1] > 1)
                continue;
            const auto &backing = scalarByOwner[i];
            if (backing.empty() ||
                row[10] != *std::min_element(ownerOrder.at(i).begin(), ownerOrder.at(i).end()))
                fail("SDBG scalar ownership mismatch");
            if (row[1] == 1)
                name = "<" + name + " return value>";
            const auto scope = "sdbg:declaration:" + std::to_string(i);
            const auto label = file < s.files.size() ? s.files.at(file).at("name").get<std::string>() + ":" +
                                                           std::to_string(line)
                                                     : "SDBG declaration " + std::to_string(i);
            result["scopes"].push_back(
                {{"id", scope}, {"name", label}, {"kind", "sdbg_declaration"}, {"parent", nullptr}});
            Json variable{{"id", "sdbg:" + std::to_string(i)},
                          {"sdbg_id", i},
                          {"name", name},
                          {"scope", scope},
                          {"scope_label", "行 " + std::to_string(line) + " · #" + std::to_string(i)},
                          {"ranges", Json::array()},
                          {"scalar_first", row[10]},
                          {"scalar_count", row[9]},
                          {"return_value", row[1] == 1}};
            try {
                const auto shaped = shape(s, scopeLists, i);
                Json leaves = Json::array();
                bool wide = false;
                for (const auto &[path, offset] : shaped.leaves) {
                    const auto found = backing.find(uint32_t(offset));
                    const auto scalar = found == backing.end() ? Json(nullptr) : Json(found->second);
                    if (!scalar.is_null() && !types.contains(scalars.at(found->second)[1]))
                        throw std::runtime_error(std::to_string(scalars.at(found->second)[1]));
                    const auto [kind, width] = scalar.is_null()
                                                   ? std::pair<std::string, uint32_t>{"unknown", 4}
                                                   : types.at(scalars.at(found->second)[1]);
                    leaves.push_back({{"path", path},
                                      {"offset", offset * 4},
                                      {"type", kind},
                                      {"size", width},
                                      {"scalar", scalar}});
                    wide |= width == 8;
                }
                Json size = uint64_t(row[9]) * 4;
                if (wide) {
                    size = 0;
                    for (auto &leaf : leaves) {
                        leaf["scalar_offset"] = leaf.at("offset").get<uint64_t>() / 4;
                        leaf["offset"] = size;
                        size = size.is_null() || leaf.at("scalar").is_null()
                                   ? Json(nullptr)
                                   : Json(size.get<uint64_t>() + leaf.at("size").get<uint64_t>());
                    }
                }
                variable["type"] = {
                    {"name", "SDBG"}, {"size", size}, {"leaves", leaves}, {"vectors", shaped.vectors}};
                if (wide)
                    variable["type"]["layout"] = "packed_inspection_values";
            } catch (const std::exception &e) {
                variable["type_issue"] = e.what();
            }
            result["variables"].push_back(std::move(variable));
        }
        const auto program = readDxbcProgram(parts.at(parts.contains("SHEX") ? "SHEX" : "SHDR"));
        const auto &ops = program.instructions;
        std::map<uint64_t, Words> byOffset;
        std::vector<uint64_t> offsets;
        uint64_t offset = 8;
        for (const auto &op : ops) {
            byOffset[offset] = op;
            offsets.push_back(offset);
            offset += op.size() * 4;
        }
        std::map<uint32_t, uint64_t> numbered;
        const QRegularExpression pattern(QStringLiteral("^\\s*(\\d+)\\s+0x([0-9a-fA-F]+):"));
        // Only code addresses are needed here. D3DDisassemble can fault while
        // interpreting malformed legacy debug metadata before our validation.
        auto codeParts = readDxbcParts(raw);
        std::erase_if(codeParts, [](const auto &part) {
            return part.first == 0x47424453u || part.first == 0x42445053u; // SDBG / SPDB
        });
        const auto codeOnly = makeDxbc(codeParts);
        for (const auto &line : QString::fromStdString(systemDisassembly(codeOnly)).split('\n')) {
            const auto m = pattern.match(line);
            if (!m.hasMatch())
                continue;
            const auto number = m.captured(1).toUInt();
            const auto at = m.captured(2).toULongLong(nullptr, 16);
            if (!byOffset.contains(at) || !numbered.emplace(number, at).second)
                fail("SDBG instruction identity");
        }
        std::vector<uint64_t> addresses;
        uint32_t ordinal = 0;
        for (const auto &[i, at] : numbered) {
            if (i != ordinal++)
                fail("Noncontiguous SDBG compiler ordinals");
            const auto opcode = byOffset.at(at)[0] & 2047;
            if (opcode != 114 && opcode != 115 && opcode != 116)
                addresses.push_back(at);
        }
        Json phases = Json::array();
        std::map<uint64_t, uint32_t> phaseByOffset;
        std::map<std::optional<uint32_t>, Arrays> phaseArrays;
        if (stage == "hs")
            for (const auto &p : hullPhases(ops)) {
                const auto id = p.at("id").get<uint32_t>();
                const auto start = p.at("start").get<size_t>(), split = p.at("split").get<size_t>(),
                           end = p.at("end").get<size_t>();
                phases.push_back({{"id", id}, {"kind", p.at("kind")}});
                for (size_t j = split; j < end; ++j)
                    phaseByOffset[offsets.at(j)] = id;
                phaseArrays[id] = indexableDeclarations(Rows(ops.begin() + start, ops.begin() + split));
            }
        else
            phaseArrays[std::nullopt] = indexableDeclarations(ops);
        const auto &instructions = s.tables.at("instructions");
        if (instructions.size() > 32768 || addresses.size() != instructions.size())
            fail("SDBG complete instruction sequence mismatch or inspection limit");
        Json bindings = Json::object(), mapped = Json::object();
        std::map<std::string, std::map<uint32_t, std::string>> reverse;
        std::map<std::string, std::map<std::string, uint32_t>> occupied;
        std::set<uint32_t> arrayOwners;
        expanded = 0;
        for (size_t i = 0; i < instructions.size(); ++i) {
            const auto at = addresses[i];
            const auto &op = byOffset.at(at), &row = instructions[i];
            const auto opcode = op[0] & 2047;
            const std::optional<uint32_t> phase =
                phaseByOffset.contains(at) ? std::optional(phaseByOffset.at(at)) : std::nullopt;
            const auto &arrays = phaseArrays.at(phase);
            if (opcode == 4 || opcode == 5 || opcode == 44 || opcode == 120)
                fail("SDBG original subroutine assignments are not yet supported");
            if (row[0] != i || row[1] != opcode || row[2] > 2)
                fail("SDBG instruction opcode or output count");
            const auto active = s.uints(row[96], row[95]), accessed = s.uints(row[98], row[97]);
            size_t scopeExpansion = 0;
            for (auto v : active) {
                if (v >= scopes.size())
                    fail("SDBG instruction scope bounds");
                scopeExpansion += scopeLists[v].size();
            }
            for (auto v : accessed)
                if (v >= vars.size())
                    fail("SDBG instruction scope bounds");
            if (std::set<uint32_t>(active.begin(), active.end()).size() != active.size() ||
                scopeExpansion > 65536)
                fail("SDBG instruction scope expansion limit");
            Json assignments = Json::array();
            const uint32_t nd = noDest.contains(opcode) ? 0 : twoDest.contains(opcode) ? 2 : 1;
            if (row[2] != nd)
                fail("SDBG output count differs from DXBC");
            const auto operands = instructionOperands(op).ranges;
            for (size_t j = 0; j < std::min<size_t>(nd, operands.size()); ++j) {
                const auto [a, b] = operands[j];
                Words operand(op.begin() + a, op.begin() + b),
                    output(row.begin() + 3 + j * 45, row.begin() + 48 + j * 45);
                const auto kind = (operand.at(0) >> 12) & 255;
                if (kind == 13) {
                    if (output[1] != none)
                        fail("SDBG null destination mismatch");
                    continue;
                }
                const auto mask = (operand[0] >> 4) & 15, words = doubleDest.contains(opcode) ? 2u : 1u;
                uint32_t debugmask = 0;
                for (uint32_t c = 0; c < 4; ++c)
                    if (output[3 + c] != none)
                        debugmask |= 1u << c;
                if (words == 2 && mask != 3 && mask != 12 && mask != 15)
                    fail("SDBG double destination is not complete register pairs");
                if ((words == 1 ? mask : mask & 5) != debugmask)
                    fail("SDBG component mask differs from DXBC");
                for (uint32_t c = 0; c < 4; ++c)
                    if ((debugmask >> c & 1) && output[3 + c] != c)
                        fail("SDBG component mask differs from DXBC");
                const bool fixed = (kind == 0 || kind == 2) && operand.size() == 2 && !(operand[0] >> 31) &&
                                   (operand[0] & 3) == 2 && !((operand[0] >> 2) & 3) &&
                                   ((operand[0] >> 20) & 3) == 1 && !((operand[0] >> 22) & 7);
                Json indexed = nullptr;
                uint32_t array = 0, arrayOffset = 0;
                if (kind == 3) {
                    const auto dest = indexableDestination(operand, arrays);
                    array = dest.access.array;
                    arrayOffset = dest.access.offset;
                    const auto &relative = dest.access.relative;
                    const bool simple =
                        !relative || (relative->size() == 2 && (relative->at(0) & ~0x30u) == 0x10000a);
                    if (nd == 1 && simple && output[0] == 5 && output[2] == array &&
                        output[1] == arrayOffset &&
                        ((!relative && output[43] == none) || (relative && output[43] == relative->at(1) &&
                                                               output[44] == ((relative->at(0) >> 4) & 3))))
                        indexed = {
                            {"array", array},
                            {"offset", arrayOffset},
                            {"index_register",
                             relative ? Json("r" + std::to_string(relative->at(1))) : Json(nullptr)},
                            {"index_component", relative ? Json((relative->at(0) >> 4) & 3) : Json(nullptr)}};
                }
                for (uint32_t c = 0; c < 4; ++c) {
                    if (!(debugmask >> c & 1))
                        continue;
                    const auto v = output[7 + 9 * c];
                    if (v == none)
                        continue;
                    if (v >= scalars.size())
                        fail("SDBG assigned scalar bounds");
                    if ((scalars[v][1] == 39) != (words == 2))
                        fail("SDBG scalar width differs from original destination");
                    const auto owner = scalars[v][4];
                    Json assignment{{"scalar", v},
                                    {"register", fixed ? Json(std::string(kind == 0 ? "r" : "o") +
                                                              std::to_string(operand[1]))
                                                       : Json(nullptr)},
                                    {"component", c},
                                    {"invalidate_owner", fixed ? Json(nullptr) : Json(owner)}};
                    if (words == 2)
                        assignment["words"] = 2;
                    if (kind == 3)
                        arrayOwners.insert(owner);
                    if (!indexed.is_null()) {
                        const auto key = (stage == "hs" ? std::to_string(phaseByOffset.at(at)) + ":" : "") +
                                         std::to_string(owner);
                        const bool fresh = !bindings.contains(key);
                        if (fresh)
                            bindings[key] = {{"array", array}, {"slots", Json::object()}};
                        auto &binding = bindings[key];
                        auto &rev = reverse[key];
                        auto &used = occupied[key];
                        if (binding.at("array") != array)
                            fail("SDBG array owner spans incompatible banks");
                        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> candidates;
                        if (fresh)
                            for (auto scalar : ownerOrder.at(owner))
                                candidates.emplace_back(scalar, scalars[scalar][2], scalars[scalar][3]);
                        candidates.emplace_back(v, arrayOffset, c);
                        const auto decl = std::find_if(arrays.begin(), arrays.end(),
                                                       [&](const auto &d) { return d.array == array; });
                        if (decl == arrays.end())
                            fail("Undeclared SDBG array");
                        for (const auto &[scalar, reg, comp] : candidates) {
                            if (reg == none && comp == none)
                                continue;
                            const auto span = scalars[scalar][1] == 39 ? 2u : 1u;
                            if (reg >= decl->elements || uint64_t(comp) + span > decl->components ||
                                (span == 2 && comp % 2))
                                fail("SDBG array scalar outside declared storage");
                            for (uint32_t word = 0; word < span; ++word) {
                                const auto physical = std::to_string(reg) + ":" + std::to_string(comp + word);
                                if (used.contains(physical) && used.at(physical) != scalar)
                                    fail("Overlapping SDBG array scalar words");
                                used[physical] = scalar;
                            }
                            const auto slot = std::to_string(reg) + ":" + std::to_string(comp);
                            if (binding["slots"].contains(slot) && binding["slots"][slot] != scalar)
                                fail("Ambiguous SDBG array scalar storage");
                            if (rev.contains(scalar) && rev.at(scalar) != slot)
                                fail("Inconsistent SDBG array scalar storage");
                            binding["slots"][slot] = scalar;
                            rev[scalar] = slot;
                        }
                        auto destination = indexed;
                        destination["owner"] = owner;
                        destination["binding"] = key;
                        assignment["array_destination"] = destination;
                        assignment["invalidate_owner"] = nullptr;
                    }
                    assignments.push_back(std::move(assignment));
                }
            }
            std::set<uint32_t> visible;
            for (auto scope : active)
                visible.insert(scopeLists[scope].begin(), scopeLists[scope].end());
            expanded += visible.size() + active.size();
            if (expanded > 1000000)
                fail("SDBG instruction scope expansion limit");
            mapped[std::to_string(at)] = {{"assignments", assignments},
                                          {"scope_ids", active},
                                          {"visible_variables", visible},
                                          {"nesting_level", row[94]},
                                          {"hs_phase", phase ? Json(*phase) : Json(nullptr)}};
        }
        for (auto &point : mapped) {
            Json assignments = Json::array();
            for (const auto &assignment : point.at("assignments"))
                if (!arrayOwners.contains(scalars[assignment.at("scalar").get<size_t>()][4]) ||
                    assignment.at("register").is_null())
                    assignments.push_back(assignment);
            point["assignments"] = assignments;
        }
        if (stage == "hs") {
            std::set<uint32_t> returns;
            for (const auto &v : result.at("variables"))
                if (v.at("return_value") == true)
                    returns.insert(v.at("sdbg_id").get<uint32_t>());
            for (auto &phase : phases) {
                std::set<uint32_t> visible, written;
                for (const auto &point : mapped)
                    if (point.at("hs_phase") == phase.at("id")) {
                        for (const auto &v : point.at("visible_variables"))
                            if (!returns.contains(v.get<uint32_t>()))
                                visible.insert(v.get<uint32_t>());
                        for (const auto &a : point.at("assignments"))
                            written.insert(scalars.at(a.at("scalar").get<size_t>())[4]);
                    }
                visible.insert(written.begin(), written.end());
                phase["variable_ids"] = visible;
                for (auto &point : mapped)
                    if (point.at("hs_phase") == phase.at("id")) {
                        Json local = Json::array();
                        for (const auto &v : point.at("visible_variables"))
                            if (visible.contains(v.get<uint32_t>()))
                                local.push_back(v);
                        point["visible_variables"] = local;
                    }
            }
        }
        Json inputs = Json::array();
        const auto signature =
            parts.contains("ISGN") ? dxbc_detail::signature(parts.at("ISGN")) : Json::array();
        for (const auto &row : s.tables.at("inputs")) {
            const auto v = row[0], kind = row[1], bank = row[2], reg = row[3], component = row[4],
                       literal = row[5];
            if (v >= scalars.size())
                fail("SDBG input scalar bounds");
            Json item{{"scalar", v}, {"kind", "unavailable"}};
            if (kind == 0 && component < 4) {
                if (bank != none && reg < 32) {
                    if (stage == "gs")
                        item.update(
                            {{"kind", "register"},
                             {"register", "v[" + std::to_string(bank) + "][" + std::to_string(reg) + "]"},
                             {"component", component}});
                    else
                        item.update({{"kind", "control_point"},
                                     {"bank", bank},
                                     {"register_index", reg},
                                     {"component", component}});
                } else if (bank == none && stage == "ds" && reg == 0x3ffffffc && component < 3)
                    item.update({{"kind", "register"}, {"register", "vDomain"}, {"component", component}});
                else if (bank == none && stage == "hs" && reg == 0x3ffffffd && component == 0)
                    item.update(
                        {{"kind", "register"}, {"register", "vOutputControlPointID"}, {"component", 0}});
                else if (bank == none && (stage == "hs" || stage == "ds") && reg == 0x3ffffffe &&
                         component == 0)
                    item["kind"] = "primitive_id";
                else if (bank == none && stage == "ds" && reg < 32)
                    item.update({{"kind", "register"},
                                 {"register", "vpc" + std::to_string(reg)},
                                 {"component", component}});
                else if (bank == none && reg == none) {
                    Json candidates = Json::array();
                    for (const auto &f : signature)
                        if (f.at("register") == reg && (f.at("used_mask").get<uint32_t>() >> component & 1))
                            candidates.push_back(f);
                    if (candidates.size() == 1 && candidates.at(0).at("system_value") == 7)
                        item["kind"] = "primitive_id";
                }
            } else if (kind == 9)
                item.update({{"kind", "literal"}, {"bits", literal}});
            if (scalars[v][1] == 39)
                item = {{"scalar", v}, {"kind", "unavailable"}};
            item["owner"] = scalars[v][4];
            inputs.push_back(item);
        }
        Json owners = Json::object();
        for (const auto &[owner, values] : ownerOrder)
            owners[std::to_string(owner)] = values;
        result.update(
            {{"status", result.at("variables").empty() ? "no_local_symbols" : "available"},
             {"instruction_map", mapped},
             {"initial", inputs},
             {"phases", phases},
             {"array_bindings", bindings},
             {"scalar_owners", owners},
             {"limits",
              Json::array({"Values require a complete original stage instruction trace and represent latest "
                           "observed source assignments.",
                           "HS phases have independent entry states and assignment histories.",
                           "SDBG scope snapshots are not a reconstructed source function stack.",
                           "Indexable temporary arrays require matching SDK storage and validated original "
                           "destination addresses.",
                           "Double assignments require both words of one validated original destination "
                           "pair; double entry inputs remain unavailable.",
                           "Ambiguous control-point inputs, unmapped indexed outputs, other multiword types "
                           "and uncaptured constant/resource inputs remain unavailable."})}});
    } catch (const std::exception &e) {
        result.update({{"status", "invalid_or_unsupported_symbols"},
                       {"variables", Json::array()},
                       {"scopes", Json::array()}});
        result["issues"].push_back(e.what());
    }
    return result;
}
Json sdbgDisplay(const Json &model, const SdbgState &state, uint64_t offset) {
    const auto point =
        model.value("instruction_map", Json::object()).value(std::to_string(offset), Json::object());
    const auto visible = point.value("visible_variables", Json::array());
    std::map<std::string, Json> scopes;
    for (const auto &s : model.at("scopes"))
        scopes[s.at("id").get<std::string>()] = s;
    Json values = Json::array();
    for (const auto &v : model.at("variables")) {
        if (std::find(visible.begin(), visible.end(), v.at("sdbg_id")) == visible.end())
            continue;
        Json base{{"variable_id", v.at("id")},
                  {"scope_id", v.at("scope")},
                  {"scope", scopes.at(v.at("scope").get<std::string>()).at("name")},
                  {"scope_label", v.at("scope_label")},
                  {"name", v.at("name")}};
        if (!v.contains("type")) {
            base.update({{"type", "unknown"},
                         {"status", "unsupported_type"},
                         {"value", nullptr},
                         {"bits", nullptr},
                         {"references", Json::array()},
                         {"issue", v.at("type_issue")}});
            values.push_back(base);
            continue;
        }
        for (const auto &leaf : v.at("type").at("leaves")) {
            std::optional<SdbgValue> value;
            if (!leaf.at("scalar").is_null()) {
                const auto i = state.find(leaf.at("scalar").get<uint32_t>());
                if (i != state.end())
                    value = i->second;
            }
            const auto kind = leaf.at("type").get<std::string>();
            auto item = base;
            item.update(
                {{"name", v.at("name").get<std::string>() + leaf.at("path").get<std::string>()},
                 {"type", kind},
                 {"status", value ? "available" : "unavailable"},
                 {"value", value ? Json(source_detail::scalarText(kind, value->bits)) : Json(nullptr)},
                 {"bits", value ? Json(value->bits) : Json(nullptr)},
                 {"references", value ? Json::array({value->reference}) : Json::array()}});
            values.push_back(item);
        }
    }
    return values;
}
SdbgTraceValues::SdbgTraceValues(const Json &result, Json rows,
                                 const std::function<Json(size_t)> &readRegisters)
    : model_(result.at("source_variables")), rows_(std::move(rows)) {
    if (model_.value("format", "") != "SDBG assignments" || model_.value("status", "") != "available" ||
        !result.value("trace", false))
        fail("SDBG values require a complete supported trace");
    if (model_.at("shader_sha256") != result.at("shader_sha256"))
        fail("SDBG trace shader identity");
    std::map<uint32_t, uint32_t> scalarWords;
    for (const auto &v : model_.at("variables"))
        for (const auto &leaf : v.value("type", Json::object()).value("leaves", Json::array()))
            if (!leaf.at("scalar").is_null())
                scalarWords[leaf.at("scalar").get<uint32_t>()] = leaf.at("size").get<uint32_t>() / 4;
    for (const auto &e : result.at("catalog"))
        if (!e.at("instruction").is_null())
            entries_[e.at("instruction").get<uint64_t>()] = e;
    std::optional<std::pair<Key, Json>> previous;
    std::map<std::string, Json> previousRegs;
    for (size_t index = 0; index < rows_.size(); ++index) {
        const auto &row = rows_.at(index);
        const auto ins = row.value("instruction", Json(nullptr));
        if (!ins.is_number_integer() || !entries_.contains(ins.get<uint64_t>()))
            fail("SDBG trace instruction identity mismatch");
        const auto &entry = entries_.at(ins.get<uint64_t>());
        if (entry.at("checkpoint_allowed") != true ||
            row.value("token", Json(nullptr)) != entry.at("token") ||
            row.value("opcode", Json(nullptr)) != entry.at("opcode"))
            fail("SDBG trace instruction identity mismatch");
        const auto phaseId = row.value("hs_phase", Json(nullptr));
        if (model_.value("shader_stage", "") == "hs" &&
            (phaseId != entry.value("hs_phase", Json(nullptr)) ||
             phaseId != result.at("register_capture").at("hs_phase").at("id")))
            fail("SDBG trace phase identity mismatch");
        const Key key{row.at("invocation").get<uint64_t>(),
                      phaseId.is_null() ? std::nullopt : std::optional(phaseId.get<uint32_t>())};
        auto &history = histories_[key];
        std::map<std::string, Json> regs;
        for (const auto &r : readRegisters(index))
            regs[r.at("name").get<std::string>()] = r;
        auto get = [&](const std::string &name, uint32_t c) -> std::optional<SdbgValue> {
            if (c >= 4)
                fail("SDBG assignment component bounds");
            const auto r = regs.find(name);
            if (r == regs.end() || r->second.at("written").at(c) != true)
                return {};
            return SdbgValue{r->second.at("bits").at(c).get<uint32_t>(), name + "." + "xyzw"[c]};
        };
        auto assigned = [&](const std::string &name, uint32_t component,
                            uint32_t words) -> std::optional<SdbgValue> {
            if (words < 1 || words > 2 || uint64_t(component) + words > 4)
                fail("SDBG assignment word bounds");
            SdbgValue result{0, ""};
            for (uint32_t j = 0; j < words; ++j) {
                const auto v = get(name, component + j);
                if (!v)
                    return {};
                result.bits |= v->bits << (j * 32);
                if (j)
                    result.reference += '/';
                result.reference += v->reference;
            }
            return result;
        };
        if (!previous || key != previous->first) {
            if (initial_.contains(key))
                fail("SDBG invocation history is not contiguous");
            Json first = nullptr;
            for (const auto &e : result.at("catalog"))
                if (e.at("checkpoint_allowed") == true && e.value("hs_phase", Json(nullptr)) == phaseId) {
                    first = e.at("instruction");
                    break;
                }
            if (row.at("hit") != 0 || first.is_null() || ins != first)
                fail("SDBG trace starts with missing history");
            Json phase = nullptr;
            for (const auto &p : model_.value("phases", Json::array()))
                if (p.at("id") == phaseId) {
                    phase = p;
                    break;
                }
            SdbgState state;
            for (const auto &v : model_.at("initial")) {
                if (!phase.is_null() &&
                    std::find(phase.at("variable_ids").begin(), phase.at("variable_ids").end(),
                              v.at("owner")) == phase.at("variable_ids").end())
                    continue;
                std::optional<SdbgValue> value;
                const auto kind = v.at("kind").get<std::string>();
                if (kind == "register")
                    value = get(v.at("register"), v.at("component"));
                else if (kind == "primitive_id" && !row.value("primitive_id", Json(nullptr)).is_null())
                    value = SdbgValue{row.at("primitive_id").get<uint32_t>(), "vPrim"};
                else if (kind == "literal")
                    value = SdbgValue{v.at("bits").get<uint32_t>(), "SDBG literal"};
                else if (kind == "control_point") {
                    const std::vector<std::string> prefixes =
                        model_.at("shader_stage") == "ds" ? std::vector<std::string>{"vicp"}
                        : !phase.is_null() && phase.at("kind") == "control_points"
                            ? std::vector<std::string>{"v"}
                            : std::vector<std::string>{"vicp", "vocp"};
                    std::vector<SdbgValue> candidates;
                    for (const auto &prefix : prefixes) {
                        const auto name = prefix + "[" + std::to_string(v.at("bank").get<uint32_t>()) + "][" +
                                          std::to_string(v.at("register_index").get<uint32_t>()) + "]";
                        if (const auto found = get(name, v.at("component")))
                            candidates.push_back(*found);
                    }
                    if (candidates.size() == 1)
                        value = candidates.front();
                }
                state[v.at("scalar").get<uint32_t>()] = value;
            }
            initial_[key] = std::move(state);
        } else {
            if (row.at("hit").get<uint64_t>() != previous->second.at("hit").get<uint64_t>() + 1)
                fail("SDBG trace is discontinuous");
            const auto &before = entries_.at(previous->second.at("instruction").get<uint64_t>());
            const auto offset = std::to_string(before.at("word_offset").get<uint64_t>() * 4);
            if (!model_.at("instruction_map").contains(offset))
                fail("SDBG trace instruction has no assignment table");
            SdbgState changes;
            const bool originalCall = row.value("call_depth", 0u) || previous->second.value("call_depth", 0u);
            auto invalidate = [&](const std::string &owner) {
                for (const auto &scalar : model_.at("scalar_owners").at(owner))
                    changes[scalar.get<uint32_t>()] = std::nullopt;
            };
            auto update = [&](uint32_t scalar, const std::optional<SdbgValue> &value) {
                if (!changes.contains(scalar) || changes.at(scalar) == value)
                    changes[scalar] = value;
                else
                    changes[scalar] = std::nullopt;
            };
            for (const auto &a : model_.at("instruction_map").at(offset).at("assignments")) {
                if (a.contains("array_destination")) {
                    const auto &dest = a.at("array_destination");
                    const auto owner = std::to_string(dest.at("owner").get<uint32_t>());
                    auto element = dest.at("offset").get<uint32_t>();
                    const auto reg = dest.at("index_register");
                    const auto ir =
                        reg.is_null() ? previousRegs.end() : previousRegs.find(reg.get<std::string>());
                    const bool known =
                        reg.is_null() ||
                        (ir != previousRegs.end() &&
                         ir->second.at("written").at(dest.at("index_component").get<size_t>()) == true);
                    if (ir != previousRegs.end() && known)
                        element += ir->second.at("bits")
                                       .at(dest.at("index_component").get<size_t>())
                                       .get<uint32_t>();
                    const auto &binding =
                        model_.at("array_bindings").at(dest.at("binding").get<std::string>());
                    const auto scalar =
                        known
                            ? binding.at("slots").value(std::to_string(element) + ":" +
                                                            std::to_string(a.at("component").get<uint32_t>()),
                                                        Json(nullptr))
                            : Json(nullptr);
                    if (scalar.is_null() || originalCall)
                        invalidate(owner);
                    else {
                        const auto id = scalar.get<uint32_t>(), words = a.value("words", 1u);
                        std::optional<SdbgValue> value;
                        if (!scalarWords.contains(id) || scalarWords.at(id) == words)
                            value = assigned("x" + std::to_string(dest.at("array").get<uint32_t>()) + "[" +
                                                 std::to_string(element) + "]",
                                             a.at("component"), words);
                        update(id, value);
                    }
                    continue;
                }
                auto value = a.at("register").is_null()
                                 ? std::optional<SdbgValue>{}
                                 : assigned(a.at("register"), a.at("component"), a.value("words", 1u));
                if (originalCall)
                    value.reset();
                if (!a.at("invalidate_owner").is_null())
                    invalidate(std::to_string(a.at("invalidate_owner").get<uint32_t>()));
                update(a.at("scalar"), value);
            }
            for (const auto &[scalar, value] : changes) {
                history[scalar].emplace_back(index, value);
                if (++count_ > 1000000)
                    fail("SDBG assignment history exceeds inspection limit");
            }
        }
        previous = std::pair{key, row};
        previousRegs = std::move(regs);
    }
}
Json SdbgTraceValues::at(size_t index) const {
    const auto &row = rows_.at(index);
    const auto phase = row.value("hs_phase", Json(nullptr));
    const Key key{row.at("invocation").get<uint64_t>(),
                  phase.is_null() ? std::nullopt : std::optional(phase.get<uint32_t>())};
    auto state = initial_.at(key);
    for (const auto &[scalar, events] : histories_.at(key)) {
        const auto it = std::upper_bound(events.begin(), events.end(), index,
                                         [](size_t i, const auto &event) { return i < event.first; });
        if (it != events.begin())
            state[scalar] = std::prev(it)->second;
    }
    return sdbgDisplay(model_, state,
                       entries_.at(row.at("instruction").get<uint64_t>()).at("word_offset").get<uint64_t>() *
                           4);
}
} // namespace flora
