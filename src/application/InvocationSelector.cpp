#include "InvocationSelector.h"
#include <QFile>
#include <QSaveFile>
#include <QString>
#include <limits>

namespace flora::checkpoint {
namespace {
constexpr auto format = "FloraGPA native input selector 1";
constexpr size_t maxBytes = 1024 * 1024;
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
bool uint32(const Json &value) {
    return value.is_number_integer() &&
           (value.is_number_unsigned() ? value.get<uint64_t>() <= UINT32_MAX
                                       : value.get<int64_t>() >= 0 && value.get<int64_t>() <= UINT32_MAX);
}
bool fields(const Json &object, const std::set<std::string> &wanted) {
    if (!object.is_object() || object.size() != wanted.size())
        return false;
    for (const auto &key : wanted)
        if (!object.contains(key))
            return false;
    return true;
}
void policy(const Json &value) {
    if (value != "unique" && value != "all")
        fail("Input match policy must be unique or all");
}
Json phaseIdentity(const Json &phase) { return {{"id", phase.at("id")}, {"kind", phase.at("kind")}}; }
uint32_t word(Bytes data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4)
        fail("Truncated input selector record");
    return Reader(data.subspan(offset)).read<uint32_t>();
}
} // namespace

std::vector<InputKey> inputKeys(const Json &inputSlots, const Json &known) {
    std::vector<InputKey> result;
    for (const auto &slot : inputSlots) {
        const auto kind = slot.at("kind").get<std::string>();
        if (kind == "temporary" || kind == "output" || kind == "indexable_temporary")
            continue;
        const auto mask = slot.at("mask").get<uint32_t>();
        for (uint32_t c = 0; c < 4; ++c)
            if (mask & (1u << c))
                result.emplace_back(slot.at("name").get<std::string>(), c);
    }
    if (known.contains("primitive"))
        result.emplace_back("vPrim", 0);
    if (known.contains("gs_instance"))
        result.emplace_back("vGSInstanceID", 0);
    return result;
}
Json validateSelector(const Json &selector, Bytes shader, const std::string &stage, const Json &inputSlots,
                      const Json &known, const Json &phase) {
    std::set<std::string> wantedFields{"format", "shader_stage", "shader_sha256", "inputs", "match_policy"};
    if (stage == "hs")
        wantedFields.insert("hs_phase");
    if (!fields(selector, wantedFields) || selector.at("format") != format)
        fail("Unsupported native input selector format");
    if (selector.at("shader_stage") != stage || selector.at("shader_sha256") != flora::sha256(shader))
        fail("Input selector does not match the original shader");
    if (stage == "hs") {
        const auto &value = selector.at("hs_phase");
        if (!fields(value, {"id", "kind"}) || !value.at("id").is_number_integer() || phase.is_null() ||
            value != phaseIdentity(phase))
            fail("Input selector does not match the original hull phase");
    }
    policy(selector.at("match_policy"));
    const auto &terms = selector.at("inputs");
    const auto wanted = inputKeys(inputSlots, known);
    if (!terms.is_array() || terms.empty() || terms.size() > 8192 || terms.size() != wanted.size())
        fail("Input selector must include every declared input component");
    std::map<InputKey, uint32_t> seen;
    for (const auto &term : terms) {
        if (!fields(term, {"name", "component", "bits"}) || !term.at("name").is_string() ||
            !uint32(term.at("component")) || term.at("component").get<uint32_t>() >= 4 ||
            !uint32(term.at("bits")))
            fail("Invalid selector component or uint32 bit value");
        InputKey key{term.at("name"), term.at("component")};
        if (!seen.emplace(std::move(key), term.at("bits").get<uint32_t>()).second)
            fail("Duplicate input selector component");
    }
    Json ordered = Json::array();
    for (const auto &[name, c] : wanted) {
        auto found = seen.find({name, c});
        if (found == seen.end())
            fail("Input selector contains missing or extra components");
        ordered.push_back({{"name", name}, {"component", c}, {"bits", found->second}});
    }
    Json result{{"format", format},
                {"shader_stage", stage},
                {"shader_sha256", selector.at("shader_sha256")},
                {"match_policy", selector.at("match_policy")},
                {"inputs", ordered}};
    if (stage == "hs")
        result["hs_phase"] = phaseIdentity(phase);
    return result;
}
Json selectorFromSnapshot(const Json &result, const Json &registers, const Json &row,
                          const std::string &matchPolicy) {
    const auto &meta = result.at("register_capture");
    std::map<std::string, Json> lookup;
    for (const auto &r : registers)
        lookup[r.at("name").get<std::string>()] = r;
    Json terms = Json::array();
    for (const auto &[name, c] : inputKeys(meta.at("register_slots"), meta.at("known_inputs"))) {
        Json bits;
        if (name == "vPrim" || name == "vGSInstanceID")
            bits = row.value(name == "vPrim" ? "primitive_id" : "gs_instance", Json(nullptr));
        else if (auto found = lookup.find(name); found != lookup.end()) {
            const auto &written = found->second.at("written").at(c);
            if (written == true || written == 1)
                bits = found->second.at("bits").at(c);
        }
        if (!uint32(bits))
            fail("Snapshot is missing declared input bit values");
        terms.push_back({{"name", name}, {"component", c}, {"bits", bits}});
    }
    if (terms.empty())
        fail("Original shader has no readable declared inputs");
    policy(matchPolicy);
    Json selector{{"format", format},
                  {"shader_stage", meta.at("shader_stage")},
                  {"shader_sha256", result.at("shader_sha256")},
                  {"match_policy", matchPolicy},
                  {"inputs", terms}};
    if (meta.at("shader_stage") == "hs")
        selector["hs_phase"] = phaseIdentity(meta.at("hs_phase"));
    return selector;
}
Json readSelector(const std::filesystem::path &path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        fail("Cannot read input selector");
    const auto raw = file.read(maxBytes + 1);
    if (file.error() != QFileDevice::NoError)
        fail("Cannot read input selector");
    if (size_t(raw.size()) > maxBytes)
        fail("Input selector exceeds 1 MiB");
    return Json::parse(raw.begin(), raw.end(), [](int depth, Json::parse_event_t, Json &) {
        if (depth > 128)
            fail("Input selector JSON nesting exceeds limit");
        return true;
    });
}
void writeSelector(const std::filesystem::path &path, const Json &selector) {
    const auto raw = selector.dump(2);
    if (raw.size() > maxBytes)
        fail("Input selector exceeds 1 MiB");
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly) || file.write(raw.data(), raw.size()) != qint64(raw.size()) ||
        !file.commit())
        fail("Cannot write input selector");
}
void verifySelectorRecords(Bytes data, const Json &meta) {
    const auto stride = meta.at("record_stride").get<size_t>(), count = meta.at("registers").get<size_t>();
    if (count > (256u * 1024 * 1024 - 32) / 32 || stride < 32 + count * 32 || data.size() % stride)
        fail("Invalid input selector record layout");
    const auto &inputSlots = meta.at("register_slots");
    if (inputSlots.size() != count)
        fail("Input selector register count mismatch");
    std::map<std::string, size_t> indices;
    for (size_t i = 0; i < count; ++i)
        if (!indices.emplace(inputSlots[i].at("name").get<std::string>(), i).second)
            fail("Duplicate input selector register");
    struct Check {
        size_t offset;
        std::optional<size_t> valid;
        uint32_t bits;
    };
    std::vector<Check> checks;
    for (const auto &term : meta.at("input_selector").at("inputs")) {
        if (!uint32(term.at("component")) || term.at("component").get<uint32_t>() > 3 ||
            !uint32(term.at("bits")))
            fail("Invalid input selector record check");
        const auto name = term.at("name").get<std::string>();
        const auto c = term.at("component").get<uint32_t>(), bits = term.at("bits").get<uint32_t>();
        if (name == "vPrim" || name == "vGSInstanceID")
            checks.push_back({name == "vPrim" ? size_t(8) : size_t(12), {}, bits});
        else {
            const auto offset = 32 + indices.at(name) * 16 + c * 4;
            checks.push_back({offset, offset + count * 16, bits});
        }
    }
    std::set<uint32_t> groups;
    for (size_t base = 0; base < data.size(); base += stride) {
        groups.insert(word(data, base));
        for (const auto &check : checks)
            if (word(data, base + check.offset) != check.bits ||
                (check.valid && word(data, base + *check.valid) != 1))
                fail("Returned native input bits do not match the selector");
    }
    if (groups.empty() || !uint32(meta.at("matched_invocations")) ||
        groups.size() != meta.at("matched_invocations").get<uint32_t>())
        fail("Trace invocation count does not match the input selector count");
}
} // namespace flora::checkpoint
