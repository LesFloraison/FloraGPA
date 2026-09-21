#include "NativeDebugConfig.h"
#include "SourceVariables.h"
#include <QFile>
#include <QSaveFile>
#include <bit>
#include <cmath>

namespace flora {
namespace {
using Json = nlohmann::json;
constexpr size_t maxBytes = 1024 * 1024;
[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }
const std::map<std::string, std::string> formats{{"gs-checkpoint", "FloraGPA native GS debug config 1"},
                                                 {"ds-checkpoint", "FloraGPA native DS debug config 1"},
                                                 {"hs-checkpoint", "FloraGPA native HS debug config 1"}};
bool fields(const Json &value, const std::set<std::string> &names) {
    if (!value.is_object() || value.size() != names.size())
        return false;
    for (const auto &name : names)
        if (!value.contains(name))
            return false;
    return true;
}
int64_t integer(const Json &value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<uint64_t>() > INT64_MAX))
        fail("Debugger configuration requires an integer within the supported range");
    return value.get<int64_t>();
}
std::string action(const Json &result) {
    const auto value = result.value("action", Json(nullptr));
    if (!value.is_string() || !formats.contains(value.get<std::string>()))
        fail("Missing original shader debug identity");
    return value.get<std::string>();
}
// Python's compact, sorted, ensure_ascii=False JSON is the persisted identity
// format. Its float formatting threshold differs from nlohmann::json::dump.
void canonical(std::string &out, const Json &value, unsigned depth = 0) {
    if (depth > 128)
        fail("Debugger mapping nesting exceeds limit");
    if (value.is_object()) {
        out += '{';
        bool first = true;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!first)
                out += ',';
            first = false;
            out += Json(it.key()).dump();
            out += ':';
            canonical(out, it.value(), depth + 1);
        }
        out += '}';
    } else if (value.is_array()) {
        out += '[';
        bool first = true;
        for (const auto &item : value) {
            if (!first)
                out += ',';
            first = false;
            canonical(out, item, depth + 1);
        }
        out += ']';
    } else if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (std::isnan(number))
            out += "NaN";
        else if (std::isinf(number))
            out += number < 0 ? "-Infinity" : "Infinity";
        else
            out += source_detail::scalarText("double", std::bit_cast<uint64_t>(number));
    } else
        out += value.dump();
}
Json parse(const QByteArray &raw) {
    auto callback = [](int depth, Json::parse_event_t, Json &) {
        if (depth > 128)
            fail("Debugger configuration JSON nesting exceeds limit");
        return true;
    };
    // json.loads(bytes) accepts BOM and zero-pattern UTF-16/32 detection.
    // Preserve that interoperability for existing exported configuration files.
    const auto size = size_t(raw.size());
    const auto *data = reinterpret_cast<const uint8_t *>(raw.constData());
    unsigned width = 1;
    bool little = false;
    if (size >= 4 && ((data[0] == 0 && data[1] == 0 && data[2] == 0xfe && data[3] == 0xff) ||
                      (data[0] == 0xff && data[1] == 0xfe && data[2] == 0 && data[3] == 0))) {
        width = 4;
        little = data[0] == 0xff;
    } else if (size >= 2 && ((data[0] == 0xfe && data[1] == 0xff) || (data[0] == 0xff && data[1] == 0xfe))) {
        width = 2;
        little = data[0] == 0xff;
    } else if (size >= 4) {
        if (!data[0]) {
            width = data[1] ? 2 : 4;
        } else if (!data[1]) {
            width = data[2] || data[3] ? 2 : 4;
            little = true;
        }
    } else if (size == 2) {
        if (!data[0])
            width = 2;
        else if (!data[1]) {
            width = 2;
            little = true;
        }
    }
    if (size % width)
        fail("Truncated debugger configuration encoding");
    if (width == 1)
        return Json::parse(raw.begin(), raw.end(), callback);
    auto unit = [&](size_t offset) {
        uint32_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= uint32_t(data[offset + i]) << (8 * (little ? i : width - i - 1));
        return value;
    };
    if (width == 2) {
        std::u16string text;
        for (size_t i = 0; i < size; i += 2)
            text += char16_t(unit(i));
        return Json::parse(text.begin(), text.end(), callback);
    }
    std::u32string text;
    for (size_t i = 0; i < size; i += 4)
        text += char32_t(unit(i));
    return Json::parse(text.begin(), text.end(), callback);
}
} // namespace
std::string debugConfigCanonicalJson(const Json &value) {
    std::string result;
    canonical(result, value);
    return result;
}
Json nativeDebugIdentity(const Json &result) {
    const auto kind = action(result);
    const auto digest = result.value("shader_sha256", Json(nullptr));
    if (!digest.is_string())
        fail("Missing original shader debug identity");
    const auto hash = digest.get<std::string>();
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        fail("Missing original shader debug identity");
    Json mappings = Json::object();
    for (const auto *key : {"catalog", "source_lines", "source_variables", "source_stack"})
        mappings[key] = result.value(key, Json(nullptr));
    std::string raw;
    canonical(raw, mappings);
    return {{"action", kind},
            {"shader_sha256", hash},
            {"mappings_sha256", sha256(Bytes(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()))}};
}
Json exportNativeDebugConfig(const Json &result, const NativeDebugSettings &settings) {
    Json watches = Json::array();
    for (const auto &watch : settings.watches)
        watches.push_back(watch.text());
    return {{"format", formats.at(action(result))},
            {"identity", nativeDebugIdentity(result)},
            {"source_breakpoints", settings.source.exportBreakpoints()},
            {"instruction_breakpoints", settings.instructions},
            {"watches", watches}};
}
NativeDebugSettings prepareNativeDebugConfig(const Json &result, const Json &rows,
                                             SourceTrace::ValueLoader loader, const Json &config) {
    if (!fields(config, {"format", "identity", "source_breakpoints", "instruction_breakpoints", "watches"}) ||
        config.at("format") != formats.at(action(result)))
        fail("Unsupported native shader debug configuration format");
    if (config.at("identity") != nativeDebugIdentity(result))
        fail("Configuration does not match the current original shader or source mappings");
    const auto &points = config.at("source_breakpoints"),
               &instructions = config.at("instruction_breakpoints"), &texts = config.at("watches");
    if (!points.is_array() || points.size() > 256)
        fail("At most 256 source breakpoints are supported");
    if (!instructions.is_array() || instructions.size() > 4096)
        fail("Invalid instruction breakpoints");
    std::set<uint64_t> allowed, selected;
    for (const auto &entry : result.at("catalog"))
        if (entry.at("checkpoint_allowed") == true)
            allowed.insert(entry.at("instruction").get<uint64_t>());
    for (const auto &item : instructions) {
        const auto number = integer(item);
        if (number < 0 || !allowed.contains(uint64_t(number)))
            fail("Instruction breakpoint has no executable original mapping");
        if (!selected.insert(uint64_t(number)).second)
            fail("Duplicate instruction breakpoint");
    }
    if (!texts.is_array() || texts.size() > 64)
        fail("At most 64 watch expressions are supported");
    std::vector<DebugExpression> watches;
    std::set<std::string> unique;
    for (const auto &text : texts) {
        if (!text.is_string())
            fail("Watch expressions must be strings");
        DebugExpression expression(text.get<std::string>());
        if (!unique.insert(expression.text()).second)
            fail("Duplicate watch expression");
        watches.push_back(std::move(expression));
    }
    SourceTrace source(result, rows, std::move(loader));
    std::set<SourceTrace::Point> seen;
    for (const auto &point : points) {
        if (!point.is_object() || !point.contains("file") || !point.contains("line"))
            fail("Invalid source breakpoint configuration");
        for (const auto &[key, value] : point.items())
            if (key != "file" && key != "line" && key != "condition" && key != "hit_count")
                fail("Invalid source breakpoint configuration");
        const auto file = integer(point.at("file")), line = integer(point.at("line"));
        source.validateLine(file, line);
        if (!seen.emplace(file, line).second)
            fail("Duplicate source breakpoint");
        const auto rule = point.value("hit_count", Json{{"mode", "always"}, {"count", 1}});
        if (!fields(rule, {"mode", "count"}) || !rule.at("mode").is_string())
            fail("Invalid breakpoint hit count rule");
        const auto condition = point.value("condition", Json(""));
        if (!condition.is_string())
            fail("Breakpoint condition must be a string");
        source.setRule(file, line, condition.get<std::string>(), rule.at("mode"), integer(rule.at("count")));
    }
    return {std::move(source), std::move(selected), std::move(watches)};
}
Json readNativeDebugConfig(const std::filesystem::path &path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        fail("Cannot read debugger configuration");
    const auto raw = file.read(maxBytes + 1);
    if (file.error() != QFileDevice::NoError)
        fail("Cannot read debugger configuration");
    if (size_t(raw.size()) > maxBytes)
        fail("Debugger configuration exceeds 1 MiB");
    try {
        return parse(raw);
    } catch (const Json::exception &) {
        fail("Debugger configuration is not valid JSON");
    }
}
void writeNativeDebugConfig(const std::filesystem::path &path, const Json &config) {
    const auto raw = config.dump(2);
    if (raw.size() > maxBytes)
        fail("Debugger configuration exceeds 1 MiB");
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly) || file.write(raw.data(), raw.size()) != qint64(raw.size()) ||
        !file.commit())
        fail("Cannot write debugger configuration");
}
} // namespace flora
